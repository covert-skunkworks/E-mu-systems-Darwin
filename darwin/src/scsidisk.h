//  Description:
//
//		scsidisk.h -- SCSI hard-disk targets backed by raw image files.
//
//		The Darwin has two SCSI buses, both run by the disk engine: bus 0 is
//		the "host" port (a computer can connect to it), bus 1 the "audio"
//		port with the internal drive and the rear Audio SCSI connector. Any
//		ID 0-7 on either bus can hold a disk image: a plain sector dump, 512
//		bytes per block, as a real drive would have been imaged.
//
//		The targets are independent of the controller: the controller hands
//		over a CDB and moves the data the target says the command needs.
//

#ifndef SCSIDISK_H
#define SCSIDISK_H

#include <stdint.h>

#define SD_BUSES 2
#define SD_IDS   8

enum { SD_NONE = 0, SD_IN = 1, SD_OUT = 2 };  // data phase direction

typedef struct
{
	uint8_t  status;   // SCSI status byte: 00 good, 02 check condition
	int      dir;      // SD_NONE, SD_IN (target to host), SD_OUT (host to target)
	uint8_t *data;     // the data-in bytes, or room for the data-out bytes
	uint32_t len;      // bytes of the data phase
} sd_xfer;

/**
  * @brief  Put a disk image on the bus (the file is opened read / write).
  *
  * @param  bus 0 host, 1 audio.
  * @param  id SCSI ID 0-7.
  * @param  path The image file.
  *
  * @retval 0 attached, -1 bad bus / ID, -2 the file cannot be opened.
  */
int sd_attach( int bus, int id, const char *path );

/**
  * @brief  Take a disk off the bus.
  *
  * @param  bus 0 host, 1 audio.
  * @param  id SCSI ID 0-7.
  *
  * @retval None.
  */
void sd_detach( int bus, int id );

/**
  * @brief  Does a target answer selection at this ID?
  *
  * @param  bus 0 host, 1 audio.
  * @param  id SCSI ID 0-7.
  *
  * @retval 1 present, 0 not.
  */
int sd_present( int bus, int id );

/**
  * @brief  Execute a command up to its data phase.
  *
  * For SD_OUT the caller fills x->data with x->len bytes and then calls
  * sd_finish(); for SD_IN and SD_NONE x->status is already final.
  *
  * @param  bus 0 host, 1 audio.
  * @param  id SCSI ID 0-7.
  * @param  cdb The command block.
  * @param  x Receives the data phase and status.
  *
  * @retval 0 on success, -1 no target at that ID.
  */
int sd_command( int bus, int id, const uint8_t *cdb, sd_xfer *x );

/**
  * @brief  Complete a data-out command once its data has arrived.
  *
  * @param  bus 0 host, 1 audio.
  * @param  id SCSI ID 0-7.
  * @param  x The transfer from sd_command(); x->status is updated.
  *
  * @retval None.
  */
void sd_finish( int bus, int id, sd_xfer *x );

/**
  * @brief  Length of a CDB from its operation code group.
  *
  * @param  op Operation code.
  *
  * @retval 6, 10 or 12.
  */
int sd_cdb_len( uint8_t op );

/**
  * @brief  Create a blank (zero-filled) image file.
  *
  * @param  path The new file.
  * @param  megabytes Its size.
  *
  * @retval 0 created, -1 the file cannot be written.
  */
int sd_create( const char *path, unsigned megabytes );

#endif
