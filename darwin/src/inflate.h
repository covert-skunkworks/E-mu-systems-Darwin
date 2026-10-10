//  Description:
//
//		inflate.h -- raw DEFLATE (RFC 1951) decoder for the firmware zips.
//
//		The Darwin firmware ships as a zip of EPROM dumps. The emulator
//		unpacks it in memory, so nothing is extracted to disk and no zlib
//		dependency is needed: this is a small, self-contained decoder in the
//		manner of zlib's puff.c, sized for a few megabytes at most.
//

#ifndef INFLATE_H
#define INFLATE_H

#include <stddef.h>
#include <stdint.h>

/**
  * @brief  Decode one raw DEFLATE stream (no zlib or gzip wrapper).
  *
  * @param  in The compressed data.
  * @param  in_len Bytes in in.
  * @param  out Receives the decoded data.
  * @param  out_len Room in out.
  * @param  produced Receives the number of bytes decoded, may be NULL.
  *
  * @retval 0 on success, -1 input ended early, -2 output full,
  *         -3 bad block type, -4 bad stored length, -5 bad Huffman code
  *         set, -6 bad symbol or distance.
  */
int inf_inflate( const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len,
				 size_t *produced );

/**
  * @brief  CRC-32 (IEEE 802.3, as used by zip), continuing from crc.
  *
  * @param  crc 0 for a new checksum, or the result of the previous call.
  * @param  p The data.
  * @param  n Bytes in p.
  *
  * @retval The updated CRC.
  */
uint32_t inf_crc32( uint32_t crc, const uint8_t *p, size_t n );

#endif
