//  Description:
//
//		firmware.c -- load the Darwin's 68306 firmware from a zip, in memory.
//
//		Only the zip features the release files use are supported: stored
//		(method 0) and deflated (method 8) members, no encryption, no ZIP64.
//		The central directory is trusted for sizes and CRCs, because members
//		written with a data descriptor have zeros in their local headers.
//
//		  end of central directory (EOCD), last 22+ bytes
//		  off size field
//		  0     4    signature 50 4B 05 06
//		  10    2    total entries
//		  12    4    central directory size
//		  16    4    central directory offset
//
//		  central directory entry
//		  0     4    signature 50 4B 01 02
//		  8     2    flags (bit 0: encrypted)
//		  10    2    method
//		  16    4    CRC-32
//		  20    4    compressed size
//		  24    4    uncompressed size
//		  28    2    name length (n)
//		  30    2    extra length (x)
//		  32    2    comment length (c)
//		  42    4    local header offset
//		  46    n    name
//
//		  local header
//		  0     4    signature 50 4B 03 04
//		  26    2    name length
//		  28    2    extra length
//		  30+n+x     data
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "firmware.h"
#include "inflate.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define FW_HALF     ( FW_ROM_SIZE / 2 )
#define FW_MAX_FILE ( 64u << 20 )        // refuse anything absurdly large

//------------------------------------------------------------------------------
//---------------------- Local types and structures ----------------------------
//------------------------------------------------------------------------------

typedef struct
{
	uint8_t *combined;  // each NULL until found
	uint8_t *msb;
	uint8_t *lsb;
	int      members;
} fw_parts;

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static uint16_t fw_rd16le( const uint8_t *p );
static uint32_t fw_rd32le( const uint8_t *p );
static void fw_msg( char *err, int err_len, const char *fmt, ... );
static int fw_ends_with( const char *name, const char *suffix );
static int fw_read_file( const char *path, uint8_t **data, size_t *len );
static int fw_zip_members( const uint8_t *zip, size_t len, fw_parts *parts, char *err,
						   int err_len );
static void fw_version( const uint8_t *rom, char *out, size_t out_len );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Read a little-endian 16-bit value.
  *
  * @param  p The two bytes.
  *
  * @retval The value.
  */
static uint16_t fw_rd16le( const uint8_t *p ) { return (uint16_t)( p[0] | ( p[1] << 8 ) ); }

/**
  * @brief  Read a little-endian 32-bit value.
  *
  * @param  p The four bytes.
  *
  * @retval The value.
  */
static uint32_t fw_rd32le( const uint8_t *p )
{
	return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) |
		   ( (uint32_t)p[3] << 24 );
}

/**
  * @brief  Format an error message into the caller's buffer, if it gave one.
  *
  * @param  err The buffer, may be NULL.
  * @param  err_len Room in err.
  * @param  fmt printf format.
  *
  * @retval None.
  */
static void fw_msg( char *err, int err_len, const char *fmt, ... )
{
	va_list ap;

	if ( !err || err_len <= 0 )
	{
		return;
	}
	va_start( ap, fmt );
	vsnprintf( err, (size_t)err_len, fmt, ap );
	va_end( ap );
}

/**
  * @brief  Case-insensitive test for a file-name suffix.
  *
  * @param  name The name.
  * @param  suffix The suffix, lower case.
  *
  * @retval 1 if name ends with suffix, 0 if not.
  */
static int fw_ends_with( const char *name, const char *suffix )
{
	size_t n = strlen( name ), s = strlen( suffix ), i;

	if ( n < s )
	{
		return 0;
	}
	for ( i = 0; i < s; i++ )
	{
		if ( tolower( (unsigned char)name[n - s + i] ) != suffix[i] )
		{
			return 0;
		}
	}
	return 1;
}

/**
  * @brief  Read a whole file into a malloc'd buffer.
  *
  * @param  path The file.
  * @param  data Receives the buffer; the caller frees it.
  * @param  len Receives its length.
  *
  * @retval 0 on success, -1 on failure.
  */
static int fw_read_file( const char *path, uint8_t **data, size_t *len )
{
	FILE *f = fopen( path, "rb" );
	long n;

	if ( !f )
	{
		return -1;
	}
	if ( fseek( f, 0, SEEK_END ) != 0 || ( n = ftell( f ) ) <= 0 || (unsigned long)n > FW_MAX_FILE ||
		 fseek( f, 0, SEEK_SET ) != 0 )
	{
		fclose( f );
		return -1;
	}
	*data = malloc( (size_t)n );
	if ( !*data || fread( *data, 1, (size_t)n, f ) != (size_t)n )
	{
		free( *data );
		*data = NULL;
		fclose( f );
		return -1;
	}
	fclose( f );
	*len = (size_t)n;
	return 0;
}

/**
  * @brief  Unpack the firmware members of a zip held in memory.
  *
  * Every member whose name marks it as a combined image or a chip dump is
  * decompressed and checked against its CRC-32; other members are skipped.
  *
  * @param  zip The zip file contents.
  * @param  len Its length.
  * @param  parts Receives the members found (malloc'd; the caller frees them).
  * @param  err Receives a message on failure.
  * @param  err_len Room in err.
  *
  * @retval 0 on success, -3 damaged zip, -4 a member does not decompress,
  *         -5 a member fails its CRC-32.
  */
static int fw_zip_members( const uint8_t *zip, size_t len, fw_parts *parts, char *err,
						   int err_len )
{
	const uint8_t *eocd = NULL, *cd, *lh;
	uint32_t cd_off, cd_size, csize, usize, crc, lh_off;
	uint16_t entries, method, flags, nlen, xlen, clen;
	char name[260];
	uint8_t **slot, *buf;
	size_t i, pos, got;
	int e, rc;

	if ( len < 22 )
	{
		return -3;
	}
	for ( i = len - 22; ; i-- )  // the EOCD is followed by at most a 64 KB comment
	{
		if ( fw_rd32le( zip + i ) == 0x06054B50u )
		{
			eocd = zip + i;
			break;
		}
		if ( i == 0 || len - i > 22 + 0xFFFF )
		{
			break;
		}
	}
	if ( !eocd )
	{
		fw_msg( err, err_len, "no zip end-of-directory record" );
		return -3;
	}
	entries = fw_rd16le( eocd + 10 );
	cd_size = fw_rd32le( eocd + 12 );
	cd_off = fw_rd32le( eocd + 16 );
	if ( (uint64_t)cd_off + cd_size > len )
	{
		fw_msg( err, err_len, "zip directory lies outside the file" );
		return -3;
	}

	pos = cd_off;
	for ( e = 0; e < entries; e++ )
	{
		if ( pos + 46 > len || fw_rd32le( zip + pos ) != 0x02014B50u )
		{
			fw_msg( err, err_len, "damaged zip directory entry %d", e );
			return -3;
		}
		cd = zip + pos;
		flags = fw_rd16le( cd + 8 );
		method = fw_rd16le( cd + 10 );
		crc = fw_rd32le( cd + 16 );
		csize = fw_rd32le( cd + 20 );
		usize = fw_rd32le( cd + 24 );
		nlen = fw_rd16le( cd + 28 );
		xlen = fw_rd16le( cd + 30 );
		clen = fw_rd16le( cd + 32 );
		lh_off = fw_rd32le( cd + 42 );
		if ( pos + 46 + nlen > len )
		{
			return -3;
		}
		snprintf( name, sizeof name, "%.*s", (int)nlen, (const char *)cd + 46 );
		pos += 46u + nlen + xlen + clen;

		slot = NULL;
		if ( fw_ends_with( name, "_combined.bin" ) )
		{
			slot = &parts->combined;
		}
		else if ( fw_ends_with( name, "_msb.bin" ) )
		{
			slot = &parts->msb;
		}
		else if ( fw_ends_with( name, "_lsb.bin" ) )
		{
			slot = &parts->lsb;
		}
		else if ( fw_ends_with( name, ".bin" ) && usize == FW_ROM_SIZE && !parts->combined )
		{
			slot = &parts->combined;  // a lone 1 MB image under another name
		}
		if ( !slot || *slot )
		{
			continue;
		}
		if ( usize != ( slot == &parts->combined ? FW_ROM_SIZE : FW_HALF ) )
		{
			fw_msg( err, err_len, "%s: %lu bytes, expected %u", name, (unsigned long)usize,
					slot == &parts->combined ? FW_ROM_SIZE : FW_HALF );
			return -4;
		}
		if ( flags & 1 )
		{
			fw_msg( err, err_len, "%s is encrypted", name );
			return -4;
		}

		if ( (uint64_t)lh_off + 30 > len || fw_rd32le( zip + lh_off ) != 0x04034B50u )
		{
			fw_msg( err, err_len, "%s: damaged local header", name );
			return -3;
		}
		lh = zip + lh_off;
		lh_off += 30u + fw_rd16le( lh + 26 ) + fw_rd16le( lh + 28 );
		if ( (uint64_t)lh_off + csize > len )
		{
			fw_msg( err, err_len, "%s: data runs past the end of the zip", name );
			return -3;
		}

		buf = malloc( usize );
		if ( !buf )
		{
			return -4;
		}
		if ( method == 0 && csize == usize )
		{
			memcpy( buf, zip + lh_off, usize );
		}
		else if ( method == 8 )
		{
			rc = inf_inflate( zip + lh_off, csize, buf, usize, &got );
			if ( rc != 0 || got != usize )
			{
				fw_msg( err, err_len, "%s: decompression failed (%d, %lu of %lu bytes)", name, rc,
						(unsigned long)got, (unsigned long)usize );
				free( buf );
				return -4;
			}
		}
		else
		{
			fw_msg( err, err_len, "%s: unsupported zip method %u", name, method );
			free( buf );
			return -4;
		}
		if ( inf_crc32( 0, buf, usize ) != crc )
		{
			fw_msg( err, err_len, "%s: CRC-32 %08lX, the zip says %08lX", name,
					(unsigned long)inf_crc32( 0, buf, usize ), (unsigned long)crc );
			free( buf );
			return -5;
		}
		*slot = buf;
		parts->members++;
	}
	return 0;
}

/**
  * @brief  Find the version banner near the start of the image.
  *
  * @param  rom The image.
  * @param  out Receives the banner, "" if there is none.
  * @param  out_len Room in out.
  *
  * @retval None.
  */
static void fw_version( const uint8_t *rom, char *out, size_t out_len )
{
	size_t i, n;

	out[0] = 0;
	for ( i = 0x100; i + 8 < 0x400; i++ )
	{
		if ( !memcmp( rom + i, "Version ", 8 ) )
		{
			for ( n = 0; n + 1 < out_len && rom[i + n] >= 0x20 && rom[i + n] < 0x7F; n++ )
			{
				out[n] = (char)rom[i + n];
			}
			out[n] = 0;
			return;
		}
	}
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Load and verify a firmware image (zip or raw 1 MB .bin).
  *
  * @param  path The zip or .bin file.
  * @param  rom Receives the 1 MB image in 68000 byte order.
  * @param  info Receives what was loaded and the checksum figures, may be NULL.
  * @param  check 1 to require the ROM checksum to match, 0 to only report it.
  * @param  err Receives a message on failure, may be NULL.
  * @param  err_len Room in err.
  *
  * @retval 0 on success, -1 cannot read the file, -2 neither a zip nor a
  *         1 MB image, -3 damaged zip, -4 a member does not decompress,
  *         -5 a member fails its CRC-32, -6 no firmware members, -7 the chip
  *         dumps do not match the combined image, -8 bad ROM checksum.
  */
int fw_load( const char *path, uint8_t rom[FW_ROM_SIZE], fw_info *info, int check,
			 char *err, int err_len )
{
	fw_parts parts;
	fw_info local;
	uint8_t *file = NULL;
	size_t len = 0, i;
	uint32_t sum = 0;
	int rc = 0;

	memset( &parts, 0, sizeof parts );
	if ( !info )
	{
		info = &local;
	}
	memset( info, 0, sizeof *info );

	if ( fw_read_file( path, &file, &len ) != 0 )
	{
		fw_msg( err, err_len, "cannot read %s", path );
		return -1;
	}

	if ( len >= 4 && fw_rd32le( file ) == 0x04034B50u )
	{
		rc = fw_zip_members( file, len, &parts, err, err_len );
		if ( rc == 0 && !parts.combined && !( parts.msb && parts.lsb ) )
		{
			fw_msg( err, err_len, "%s holds no Darwin firmware (_combined.bin or _msb/_lsb.bin)", path );
			rc = -6;
		}
		if ( rc == 0 && parts.msb && parts.lsb )
		{
			for ( i = 0; i < FW_HALF; i++ )
			{
				if ( parts.combined && ( parts.combined[2 * i] != parts.msb[i] ||
										 parts.combined[2 * i + 1] != parts.lsb[i] ) )
				{
					fw_msg( err, err_len, "the chip dumps differ from the combined image at %06lX",
							(unsigned long)( 2 * i ) );
					rc = -7;
					break;
				}
				rom[2 * i] = parts.msb[i];  // the MSB chip drives D15-D8: even addresses
				rom[2 * i + 1] = parts.lsb[i];
			}
			snprintf( info->source, sizeof info->source, parts.combined ? "combined+msb+lsb" : "msb+lsb" );
		}
		else if ( rc == 0 )
		{
			memcpy( rom, parts.combined, FW_ROM_SIZE );
			snprintf( info->source, sizeof info->source, "combined" );
		}
		info->members = parts.members;
	}
	else if ( len == FW_ROM_SIZE )
	{
		memcpy( rom, file, FW_ROM_SIZE );
		snprintf( info->source, sizeof info->source, "raw" );
	}
	else
	{
		fw_msg( err, err_len, "%s is neither a zip nor a 1 MB image", path );
		rc = -2;
	}
	free( parts.combined );
	free( parts.msb );
	free( parts.lsb );
	free( file );
	if ( rc != 0 )
	{
		return rc;
	}

	for ( i = 0; i < FW_ROM_SIZE; i += 2 )
	{
		info->sum_msb = (uint16_t)( info->sum_msb + rom[i] );
		info->sum_lsb = (uint16_t)( info->sum_lsb + rom[i + 1] );
	}
	for ( i = 0; i < FW_ROM_SIZE; i++ )
	{
		sum += rom[i];
	}
	info->computed_sum = (uint8_t)sum;
	info->stored_sum = rom[FW_CSUM_ADDR];
	fw_version( rom, info->version, sizeof info->version );
	if ( check && info->computed_sum != info->stored_sum )
	{
		fw_msg( err, err_len, "ROM checksum %02X, the image says %02X (MSB %04X, LSB %04X)",
				info->computed_sum, info->stored_sum, info->sum_msb, info->sum_lsb );
		return -8;
	}
	return 0;
}
