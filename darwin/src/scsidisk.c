//  Description:
//
//		scsidisk.c -- SCSI hard-disk targets backed by raw image files.
//
//		Command set of a plain direct-access disk of the late 1990s, enough
//		for the Darwin's drive scan, format, file system and recording:
//
//		  00 TEST UNIT READY   03 REQUEST SENSE    04 FORMAT UNIT
//		  08 READ(6)           0A WRITE(6)         0B SEEK(6)
//		  12 INQUIRY           15 MODE SELECT(6)   1A MODE SENSE(6)
//		  1B START STOP UNIT   1E PREVENT ALLOW    25 READ CAPACITY
//		  28 READ(10)          2A WRITE(10)        2B SEEK(10)
//		  2F VERIFY(10)        35 SYNC CACHE       37 READ DEFECT DATA
//
//		Writes go straight to the file (fflush per command), so an image is
//		consistent whenever the emulator stops.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "scsidisk.h"
#include "darwin.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define SD_BLOCK     512
#define SD_SMALL_BUF 65536  // allocated at attach: room for any non-READ / WRITE reply

#define SD_KEY_NOT_READY   0x02
#define SD_KEY_MEDIUM_ERR  0x03
#define SD_KEY_ILLEGAL     0x05
#define SD_KEY_PROTECT     0x07
#define SD_ASC_BAD_OPCODE  0x20
#define SD_ASC_LBA_RANGE   0x21
#define SD_ASC_BAD_FIELD   0x24
#define SD_ASC_PROTECTED   0x27

//------------------------------------------------------------------------------
//---------------------- Local types and structures ----------------------------
//------------------------------------------------------------------------------

typedef struct
{
	FILE     *f;
	char      path[260];
	uint32_t  blocks;
	int       ro;
	uint8_t   sense_key, asc;
	uint8_t  *buf;          // data phase buffer
	uint32_t  cap;
	uint32_t  wr_lba;       // pending WRITE: where the data goes
	uint32_t  wr_blocks;
} sd_disk;

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static sd_disk sd_disks[SD_BUSES][SD_IDS];

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static uint8_t *sd_reserve( sd_disk *d, uint32_t n );
static void sd_check( sd_disk *d, sd_xfer *x, uint8_t key, uint8_t asc );
static int sd_io( sd_disk *d, uint32_t lba, uint32_t n, int write );
static void sd_in( sd_xfer *x, uint8_t *data, uint32_t len, uint32_t alloc );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Make the data buffer at least n bytes.
  *
  * @param  d The disk.
  * @param  n Bytes needed.
  *
  * @retval The buffer, NULL if memory ran out.
  */
static uint8_t *sd_reserve( sd_disk *d, uint32_t n )
{
	uint8_t *p;

	if ( n > d->cap )
	{
		p = realloc( d->buf, n );
		if ( !p )
		{
			return NULL;
		}
		d->buf = p;
		d->cap = n;
	}
	return d->buf;
}

/**
  * @brief  End a command with CHECK CONDITION and the given sense.
  *
  * @param  d The disk.
  * @param  x The transfer.
  * @param  key Sense key.
  * @param  asc Additional sense code.
  *
  * @retval None.
  */
static void sd_check( sd_disk *d, sd_xfer *x, uint8_t key, uint8_t asc )
{
	d->sense_key = key;
	d->asc = asc;
	x->status = 0x02;
	x->dir = SD_NONE;
	x->len = 0;
}

/**
  * @brief  Read or write whole blocks of the image (data in d->buf).
  *
  * @param  d The disk.
  * @param  lba First block.
  * @param  n Number of blocks.
  * @param  write 1 to write.
  *
  * @retval 0 on success, -1 out of range or I/O error.
  */
static int sd_io( sd_disk *d, uint32_t lba, uint32_t n, int write )
{
	if ( (uint64_t)lba + n > d->blocks )
	{
		return -1;
	}
	if ( plat_fseek64( d->f, (int64_t)lba * SD_BLOCK, SEEK_SET ) != 0 )
	{
		return -1;
	}
	if ( write )
	{
		// a full host disk shows up at the flush, not the buffered fwrite
		if ( fwrite( d->buf, SD_BLOCK, n, d->f ) != n || fflush( d->f ) != 0 )
		{
			dw_log( "scsi: write of %u blocks at %u failed (host disk full?)\n", n, lba );
			return -1;
		}
		return 0;
	}
	return fread( d->buf, SD_BLOCK, n, d->f ) == n ? 0 : -1;
}

/**
  * @brief  Set up a data-in phase of at most the allocation length.
  *
  * @param  x The transfer.
  * @param  data The bytes.
  * @param  len How many there are.
  * @param  alloc The allocation length from the CDB.
  *
  * @retval None.
  */
static void sd_in( sd_xfer *x, uint8_t *data, uint32_t len, uint32_t alloc )
{
	x->status = 0;
	x->data = data;
	x->len = len < alloc ? len : alloc;
	x->dir = x->len ? SD_IN : SD_NONE;
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Put a disk image on the bus (the file is opened read / write).
  *
  * @param  bus 0 host, 1 audio.
  * @param  id SCSI ID 0-7.
  * @param  path The image file.
  *
  * @retval 0 attached, -1 bad bus / ID, -2 the file cannot be opened.
  */
int sd_attach( int bus, int id, const char *path )
{
	sd_disk *d;
	int64_t size;

	if ( bus < 0 || bus >= SD_BUSES || id < 0 || id >= SD_IDS )
	{
		return -1;
	}
	sd_detach( bus, id );
	d = &sd_disks[bus][id];
	d->ro = 0;
	d->f = fopen( path, "r+b" );
	if ( !d->f )
	{
		d->f = fopen( path, "rb" );
		d->ro = 1;
	}
	if ( !d->f )
	{
		return -2;
	}
	if ( !sd_reserve( d, SD_SMALL_BUF ) )  // the replies of the small commands always fit
	{
		fclose( d->f );
		d->f = NULL;
		return -2;
	}
	plat_fseek64( d->f, 0, SEEK_END );
	size = plat_ftell64( d->f );
	d->blocks = (uint32_t)( size / SD_BLOCK );
	snprintf( d->path, sizeof d->path, "%s", path );
	d->sense_key = d->asc = 0;
	dw_log( "SCSI bus %d ID %d: %s, %u blocks (%.1f MB)%s\n", bus, id, path, d->blocks,
			d->blocks / 2048.0, d->ro ? ", read-only" : "" );
	return 0;
}

/**
  * @brief  Take a disk off the bus.
  *
  * @param  bus 0 host, 1 audio.
  * @param  id SCSI ID 0-7.
  *
  * @retval None.
  */
void sd_detach( int bus, int id )
{
	sd_disk *d;

	if ( bus < 0 || bus >= SD_BUSES || id < 0 || id >= SD_IDS )
	{
		return;
	}
	d = &sd_disks[bus][id];
	if ( d->f )
	{
		fclose( d->f );
		d->f = NULL;
	}
}

/**
  * @brief  Does a target answer selection at this ID?
  *
  * @param  bus 0 host, 1 audio.
  * @param  id SCSI ID 0-7.
  *
  * @retval 1 present, 0 not.
  */
int sd_present( int bus, int id )
{
	return bus >= 0 && bus < SD_BUSES && id >= 0 && id < SD_IDS && sd_disks[bus][id].f != NULL;
}

/**
  * @brief  Length of a CDB from its operation code group.
  *
  * @param  op Operation code.
  *
  * @retval 6, 10 or 12.
  */
int sd_cdb_len( uint8_t op )
{
	switch ( op >> 5 )
	{
	case 1:
	case 2:
		return 10;
	case 5:
		return 12;
	default:
		return 6;
	}
}

/**
  * @brief  Execute a command up to its data phase.
  *
  * @param  bus 0 host, 1 audio.
  * @param  id SCSI ID 0-7.
  * @param  cdb The command block.
  * @param  x Receives the data phase and status.
  *
  * @retval 0 on success, -1 no target at that ID.
  */
int sd_command( int bus, int id, const uint8_t *cdb, sd_xfer *x )
{
	sd_disk *d;
	uint8_t *b;
	uint32_t lba, n, last, cyl;
	int page, p;

	if ( !sd_present( bus, id ) )
	{
		return -1;
	}
	d = &sd_disks[bus][id];
	memset( x, 0, sizeof *x );
	if ( dw_log_io )
	{
		dw_log( "SCSI %d:%d cmd %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", bus, id, cdb[0],
				cdb[1], cdb[2], cdb[3], cdb[4], cdb[5], cdb[6], cdb[7], cdb[8], cdb[9] );
	}
	if ( ( cdb[1] >> 5 ) != 0 && cdb[0] != 0x03 && cdb[0] != 0x12 )  // only LUN 0 exists
	{
		sd_check( d, x, SD_KEY_ILLEGAL, 0x25 );
		return 0;
	}
	switch ( cdb[0] )
	{
	case 0x00: case 0x04: case 0x0B: case 0x1B: case 0x1E: case 0x2B: case 0x2F: case 0x35:
		break;  // nothing to do: GOOD
	case 0x03:  // REQUEST SENSE
		b = d->buf;
		memset( b, 0, 18 );
		b[0] = 0x70;
		b[2] = d->sense_key;
		b[7] = 10;
		b[12] = d->asc;
		d->sense_key = d->asc = 0;
		sd_in( x, b, 18, cdb[4] ? cdb[4] : 4 );
		return 0;
	case 0x12:  // INQUIRY
		b = d->buf;
		memset( b, 0, 36 );
		b[0] = ( cdb[1] >> 5 ) ? 0x7F : 0x00;  // other LUNs: not present
		b[2] = 0x02;
		b[3] = 0x02;
		b[4] = 31;
		memcpy( b + 8, "EMU     ", 8 );
		memcpy( b + 16, "DARWIN DISK IMG ", 16 );
		memcpy( b + 32, "1.0 ", 4 );
		sd_in( x, b, 36, cdb[4] );
		return 0;
	case 0x15:  // MODE SELECT(6): accepted and ignored
		b = d->buf;
		x->dir = cdb[4] ? SD_OUT : SD_NONE;
		x->data = b;
		x->len = cdb[4];
		d->wr_blocks = 0;
		return 0;
	case 0x1A:  // MODE SENSE(6)
		b = d->buf;
		memset( b, 0, 64 );
		page = cdb[2] & 0x3F;
		p = 4;
		b[2] = d->ro ? 0x80 : 0;
		b[3] = 8;  // one block descriptor
		b[p + 1] = (uint8_t)( d->blocks >> 16 );
		b[p + 2] = (uint8_t)( d->blocks >> 8 );
		b[p + 3] = (uint8_t)d->blocks;
		b[p + 6] = SD_BLOCK >> 8;
		p += 8;
		if ( page == 0x03 || page == 0x3F )  // format page
		{
			b[p] = 0x03;
			b[p + 1] = 0x16;
			b[p + 11] = 32;
			b[p + 12] = SD_BLOCK >> 8;
			p += 24;
		}
		if ( page == 0x04 || page == 0x3F )  // rigid geometry page
		{
			cyl = d->blocks / ( 32 * 16 );
			b[p] = 0x04;
			b[p + 1] = 0x16;
			b[p + 2] = (uint8_t)( cyl >> 16 );
			b[p + 3] = (uint8_t)( cyl >> 8 );
			b[p + 4] = (uint8_t)cyl;
			b[p + 5] = 16;
			b[p + 20] = 0x15;  // 5400 rpm
			b[p + 21] = 0x18;
			p += 24;
		}
		b[0] = (uint8_t)( p - 1 );
		sd_in( x, b, (uint32_t)p, cdb[4] );
		return 0;
	case 0x25:  // READ CAPACITY
		b = d->buf;
		last = d->blocks ? d->blocks - 1 : 0;
		b[0] = (uint8_t)( last >> 24 );
		b[1] = (uint8_t)( last >> 16 );
		b[2] = (uint8_t)( last >> 8 );
		b[3] = (uint8_t)last;
		b[4] = 0;
		b[5] = 0;
		b[6] = SD_BLOCK >> 8;
		b[7] = 0;
		sd_in( x, b, 8, 8 );
		return 0;
	case 0x37:  // READ DEFECT DATA: an empty list
		b = d->buf;
		b[0] = 0;
		b[1] = cdb[2];
		b[2] = 0;
		b[3] = 0;
		sd_in( x, b, 4, ( (uint32_t)cdb[7] << 8 ) | cdb[8] );
		return 0;
	case 0x08:  // READ(6)
	case 0x28:  // READ(10)
		if ( cdb[0] == 0x08 )
		{
			lba = ( (uint32_t)( cdb[1] & 0x1F ) << 16 ) | ( cdb[2] << 8 ) | cdb[3];
			n = cdb[4] ? cdb[4] : 256;
		}
		else
		{
			lba = ( (uint32_t)cdb[2] << 24 ) | ( (uint32_t)cdb[3] << 16 ) | ( cdb[4] << 8 ) | cdb[5];
			n = ( (uint32_t)cdb[7] << 8 ) | cdb[8];
		}
		if ( !sd_reserve( d, n * SD_BLOCK + 1 ) )
		{
			sd_check( d, x, SD_KEY_MEDIUM_ERR, 0x11 );
			return 0;
		}
		if ( sd_io( d, lba, n, 0 ) != 0 )
		{
			sd_check( d, x, SD_KEY_ILLEGAL, SD_ASC_LBA_RANGE );
			return 0;
		}
		sd_in( x, d->buf, n * SD_BLOCK, n * SD_BLOCK );
		return 0;
	case 0x0A:  // WRITE(6)
	case 0x2A:  // WRITE(10)
		if ( cdb[0] == 0x0A )
		{
			lba = ( (uint32_t)( cdb[1] & 0x1F ) << 16 ) | ( cdb[2] << 8 ) | cdb[3];
			n = cdb[4] ? cdb[4] : 256;
		}
		else
		{
			lba = ( (uint32_t)cdb[2] << 24 ) | ( (uint32_t)cdb[3] << 16 ) | ( cdb[4] << 8 ) | cdb[5];
			n = ( (uint32_t)cdb[7] << 8 ) | cdb[8];
		}
		if ( d->ro )
		{
			sd_check( d, x, SD_KEY_PROTECT, SD_ASC_PROTECTED );
			return 0;
		}
		if ( (uint64_t)lba + n > d->blocks )
		{
			sd_check( d, x, SD_KEY_ILLEGAL, SD_ASC_LBA_RANGE );
			return 0;
		}
		if ( !sd_reserve( d, n * SD_BLOCK + 1 ) )
		{
			sd_check( d, x, SD_KEY_MEDIUM_ERR, 0x0C );
			return 0;
		}
		d->wr_lba = lba;
		d->wr_blocks = n;
		x->dir = n ? SD_OUT : SD_NONE;
		x->data = d->buf;
		x->len = n * SD_BLOCK;
		return 0;
	default:
		dw_log( "SCSI %d:%d unsupported command %02X\n", bus, id, cdb[0] );
		sd_check( d, x, SD_KEY_ILLEGAL, SD_ASC_BAD_OPCODE );
		return 0;
	}
	x->status = 0;
	return 0;
}

/**
  * @brief  Complete a data-out command once its data has arrived.
  *
  * @param  bus 0 host, 1 audio.
  * @param  id SCSI ID 0-7.
  * @param  x The transfer from sd_command(); x->status is updated.
  *
  * @retval None.
  */
void sd_finish( int bus, int id, sd_xfer *x )
{
	sd_disk *d;

	if ( !sd_present( bus, id ) )
	{
		return;
	}
	d = &sd_disks[bus][id];
	x->status = 0;
	if ( d->wr_blocks && sd_io( d, d->wr_lba, d->wr_blocks, 1 ) != 0 )
	{
		sd_check( d, x, SD_KEY_MEDIUM_ERR, 0x0C );
	}
	d->wr_blocks = 0;
}

/**
  * @brief  Create a blank (zero-filled) image file.
  *
  * @param  path The new file.
  * @param  megabytes Its size.
  *
  * @retval 0 created, -1 the file cannot be written.
  */
int sd_create( const char *path, unsigned megabytes )
{
	// sparse: blocks the Darwin never writes take no disk space (it wants 275 MB at least, and a
	// recording session touches only part of that)
	return plat_sparse_file( path, (uint64_t)megabytes * 1048576 );
}
