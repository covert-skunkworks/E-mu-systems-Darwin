//  Description:
//
//		firmware.h -- load the Darwin's 68306 firmware from a zip, in memory.
//
//		The firmware is two 27C040 EPROMs (512 KB each) on a 16-bit bus: the
//		MSB chip holds the even bytes, the LSB chip the odd ones. The release
//		zips carry both chip dumps and a combined 1 MB image:
//
//		  emu_darwin_v2_50.zip
//		    emu_darwin_v2_50_combined.bin   1 MB, 68000 order
//		    emu_darwin_v2_50_msb.bin        512 KB, even bytes
//		    emu_darwin_v2_50_lsb.bin        512 KB, odd bytes
//
//		Nothing is extracted to disk. Each member's zip CRC-32 is checked,
//		the chip dumps must interleave to the combined image, and the image
//		must pass the firmware's own ROM test (diagnostics "68K ROM Test",
//		ROM routine 057CCA): the sum of all bytes, modulo 256, equals the
//		byte at 000140. A plain 1 MB .bin is accepted too.
//

#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

#define FW_ROM_SIZE  0x100000  // 1 MB: 2 x 27C040
#define FW_CSUM_ADDR 0x000140  // checksum byte (the ROM test compares the low byte)

typedef struct
{
	char     source[32];    // "combined", "msb+lsb", "combined+msb+lsb", "raw"
	char     version[64];   // e.g. "Version 2.5 Aug 14, 1998"
	uint8_t  stored_sum;    // byte at FW_CSUM_ADDR
	uint8_t  computed_sum;  // sum of all bytes, low byte
	uint16_t sum_msb;       // the ROM test's figures: 16-bit sum of the even bytes
	uint16_t sum_lsb;       // ... and of the odd bytes
	int      members;       // zip members read (0 for a raw image)
} fw_info;

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
			 char *err, int err_len );

#endif
