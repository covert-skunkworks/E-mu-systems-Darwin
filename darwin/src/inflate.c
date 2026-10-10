//  Description:
//
//		inflate.c -- raw DEFLATE (RFC 1951) decoder for the firmware zips.
//
//		The structure follows zlib's puff.c: a canonical Huffman decoder that
//		walks the code one bit at a time. That is slower than table-driven
//		decoding, but the firmware is about a megabyte and is unpacked once
//		at start-up, so simplicity wins over speed.
//
//		DEFLATE bit order: bits are taken from each byte LSB first; Huffman
//		codes are stored MSB first, extra bits and header fields LSB first.
//
//		  block header  BFINAL(1) BTYPE(2): 0 stored, 1 fixed, 2 dynamic
//		  stored        align, LEN(16) NLEN(16), LEN raw bytes
//		  dynamic       HLIT(5) HDIST(5) HCLEN(4), code-length codes,
//		                literal/length + distance code lengths
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "inflate.h"

#include <string.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define INF_MAXBITS   15                               // longest Huffman code
#define INF_MAXLCODES 286                              // literal/length codes
#define INF_MAXDCODES 30                               // distance codes
#define INF_MAXCODES  ( INF_MAXLCODES + INF_MAXDCODES )
#define INF_FIXLCODES 288                              // fixed table size

#define INF_E_INPUT   -1
#define INF_E_OUTPUT  -2
#define INF_E_TYPE    -3
#define INF_E_STORED  -4
#define INF_E_CODES   -5
#define INF_E_SYMBOL  -6

//------------------------------------------------------------------------------
//---------------------- Local types and structures ----------------------------
//------------------------------------------------------------------------------

typedef struct
{
	const uint8_t *in;
	size_t         in_len;
	size_t         in_pos;
	uint32_t       bitbuf;   // bits not yet used, LSB first
	int            bitcnt;
	uint8_t       *out;
	size_t         out_len;
	size_t         out_pos;
	int            err;      // sticky: set when the input runs out
} inf_state;

typedef struct
{
	int16_t *count;   // number of codes of each length
	int16_t *symbol;  // symbols in canonical code order
} inf_huff;

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static const int16_t inf_lbase[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const int16_t inf_lext[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const int16_t inf_dbase[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
	8193, 12289, 16385, 24577 };
static const int16_t inf_dext[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

// order of the code-length code lengths in a dynamic block header
static const uint8_t inf_order[19] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static uint32_t inf_crc_table[256];
static int      inf_crc_ready;

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static int inf_bits( inf_state *s, int need );
static int inf_stored( inf_state *s );
static int inf_decode( inf_state *s, const inf_huff *h );
static int inf_construct( inf_huff *h, const int16_t *length, int n );
static int inf_codes( inf_state *s, const inf_huff *lencode, const inf_huff *distcode );
static int inf_fixed( inf_state *s );
static int inf_dynamic( inf_state *s );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Take need bits from the input, LSB first.
  *
  * Running out of input sets s->err and returns 0, so callers can check the
  * sticky error once after a group of reads.
  *
  * @param  s Decoder state.
  * @param  need Number of bits, 0 to 13.
  *
  * @retval The bits, right-aligned.
  */
static int inf_bits( inf_state *s, int need )
{
	uint32_t val = s->bitbuf;

	while ( s->bitcnt < need )
	{
		if ( s->in_pos >= s->in_len )
		{
			s->err = INF_E_INPUT;
			return 0;
		}
		val |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = val >> need;
	s->bitcnt -= need;
	return (int)( val & ( ( 1u << need ) - 1 ) );
}

/**
  * @brief  Copy a stored (uncompressed) block.
  *
  * @param  s Decoder state.
  *
  * @retval 0 on success, or a negative INF_E_ code.
  */
static int inf_stored( inf_state *s )
{
	unsigned len, nlen;

	// a stored block starts on a byte boundary: the rest of the byte is padding
	s->bitbuf = 0;
	s->bitcnt = 0;
	if ( s->in_pos + 4 > s->in_len )
	{
		return INF_E_INPUT;
	}
	len = s->in[s->in_pos] | ( s->in[s->in_pos + 1] << 8 );
	nlen = s->in[s->in_pos + 2] | ( s->in[s->in_pos + 3] << 8 );
	s->in_pos += 4;
	if ( len != ( ~nlen & 0xFFFFu ) )
	{
		return INF_E_STORED;
	}
	if ( s->in_pos + len > s->in_len )
	{
		return INF_E_INPUT;
	}
	if ( s->out_pos + len > s->out_len )
	{
		return INF_E_OUTPUT;
	}
	memcpy( s->out + s->out_pos, s->in + s->in_pos, len );
	s->out_pos += len;
	s->in_pos += len;
	return 0;
}

/**
  * @brief  Decode one symbol with a canonical Huffman code.
  *
  * @param  s Decoder state.
  * @param  h The code.
  *
  * @retval The symbol, or a negative value if the input ran out or the bits
  *         match no code.
  */
static int inf_decode( inf_state *s, const inf_huff *h )
{
	int len, code = 0, first = 0, count, index = 0;

	for ( len = 1; len <= INF_MAXBITS; len++ )
	{
		code |= inf_bits( s, 1 );
		if ( s->err )
		{
			return s->err;
		}
		count = h->count[len];
		if ( code - count < first )  // code < first + count: it has this length
		{
			return h->symbol[index + ( code - first )];
		}
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return INF_E_SYMBOL;
}

/**
  * @brief  Build a canonical Huffman code from a list of code lengths.
  *
  * @param  h Receives the code; count[] and symbol[] must be big enough.
  * @param  length Code length of each symbol, 0 = unused.
  * @param  n Number of symbols.
  *
  * @retval 0 complete code, > 0 incomplete, < 0 over-subscribed.
  */
static int inf_construct( inf_huff *h, const int16_t *length, int n )
{
	int16_t offs[INF_MAXBITS + 1];
	int symbol, len, left;

	for ( len = 0; len <= INF_MAXBITS; len++ )
	{
		h->count[len] = 0;
	}
	for ( symbol = 0; symbol < n; symbol++ )
	{
		h->count[length[symbol]]++;
	}
	if ( h->count[0] == n )  // no codes at all: complete, but decode fails
	{
		return 0;
	}
	left = 1;
	for ( len = 1; len <= INF_MAXBITS; len++ )
	{
		left <<= 1;
		left -= h->count[len];
		if ( left < 0 )
		{
			return left;
		}
	}
	offs[1] = 0;
	for ( len = 1; len < INF_MAXBITS; len++ )
	{
		offs[len + 1] = (int16_t)( offs[len] + h->count[len] );
	}
	for ( symbol = 0; symbol < n; symbol++ )
	{
		if ( length[symbol] != 0 )
		{
			h->symbol[offs[length[symbol]]++] = (int16_t)symbol;
		}
	}
	return left;
}

/**
  * @brief  Decode literals and length/distance pairs up to end-of-block.
  *
  * @param  s Decoder state.
  * @param  lencode Literal/length code.
  * @param  distcode Distance code.
  *
  * @retval 0 on success, or a negative INF_E_ code.
  */
static int inf_codes( inf_state *s, const inf_huff *lencode, const inf_huff *distcode )
{
	int symbol, len;
	size_t dist;

	do
	{
		symbol = inf_decode( s, lencode );
		if ( symbol < 0 )
		{
			return s->err ? s->err : INF_E_SYMBOL;
		}
		if ( symbol < 256 )
		{
			if ( s->out_pos >= s->out_len )
			{
				return INF_E_OUTPUT;
			}
			s->out[s->out_pos++] = (uint8_t)symbol;
		}
		else if ( symbol > 256 )
		{
			symbol -= 257;
			if ( symbol >= 29 )
			{
				return INF_E_SYMBOL;
			}
			len = inf_lbase[symbol] + inf_bits( s, inf_lext[symbol] );
			symbol = inf_decode( s, distcode );
			if ( symbol < 0 || symbol >= 30 )
			{
				return s->err ? s->err : INF_E_SYMBOL;
			}
			dist = (size_t)( inf_dbase[symbol] + inf_bits( s, inf_dext[symbol] ) );
			if ( s->err )
			{
				return s->err;
			}
			if ( dist > s->out_pos )  // reaches back before the start
			{
				return INF_E_SYMBOL;
			}
			if ( s->out_pos + (size_t)len > s->out_len )
			{
				return INF_E_OUTPUT;
			}
			while ( len-- )  // byte by byte: source and destination may overlap
			{
				s->out[s->out_pos] = s->out[s->out_pos - dist];
				s->out_pos++;
			}
		}
	} while ( symbol != 256 );
	return 0;
}

/**
  * @brief  Decode a block compressed with the fixed Huffman codes.
  *
  * @param  s Decoder state.
  *
  * @retval 0 on success, or a negative INF_E_ code.
  */
static int inf_fixed( inf_state *s )
{
	static int16_t  lencnt[INF_MAXBITS + 1], lensym[INF_FIXLCODES];
	static int16_t  distcnt[INF_MAXBITS + 1], distsym[INF_MAXDCODES];
	static inf_huff lencode = { lencnt, lensym }, distcode = { distcnt, distsym };
	static int      built;
	int16_t lengths[INF_FIXLCODES];
	int symbol;

	if ( !built )
	{
		for ( symbol = 0; symbol < 144; symbol++ )
		{
			lengths[symbol] = 8;
		}
		for ( ; symbol < 256; symbol++ )
		{
			lengths[symbol] = 9;
		}
		for ( ; symbol < 280; symbol++ )
		{
			lengths[symbol] = 7;
		}
		for ( ; symbol < INF_FIXLCODES; symbol++ )
		{
			lengths[symbol] = 8;
		}
		inf_construct( &lencode, lengths, INF_FIXLCODES );
		for ( symbol = 0; symbol < INF_MAXDCODES; symbol++ )
		{
			lengths[symbol] = 5;
		}
		inf_construct( &distcode, lengths, INF_MAXDCODES );
		built = 1;
	}
	return inf_codes( s, &lencode, &distcode );
}

/**
  * @brief  Decode a block that carries its own Huffman codes.
  *
  * @param  s Decoder state.
  *
  * @retval 0 on success, or a negative INF_E_ code.
  */
static int inf_dynamic( inf_state *s )
{
	int16_t  lengths[INF_MAXCODES];
	int16_t  lencnt[INF_MAXBITS + 1], lensym[INF_MAXLCODES];
	int16_t  distcnt[INF_MAXBITS + 1], distsym[INF_MAXDCODES];
	inf_huff lencode = { lencnt, lensym }, distcode = { distcnt, distsym };
	int nlen, ndist, ncode, index, symbol, len, err;

	nlen = inf_bits( s, 5 ) + 257;
	ndist = inf_bits( s, 5 ) + 1;
	ncode = inf_bits( s, 4 ) + 4;
	if ( s->err )
	{
		return s->err;
	}
	if ( nlen > INF_MAXLCODES || ndist > INF_MAXDCODES )
	{
		return INF_E_CODES;
	}

	for ( index = 0; index < ncode; index++ )
	{
		lengths[inf_order[index]] = (int16_t)inf_bits( s, 3 );
	}
	for ( ; index < 19; index++ )
	{
		lengths[inf_order[index]] = 0;
	}
	if ( s->err )
	{
		return s->err;
	}
	if ( inf_construct( &lencode, lengths, 19 ) != 0 )  // must be complete
	{
		return INF_E_CODES;
	}

	index = 0;
	while ( index < nlen + ndist )
	{
		symbol = inf_decode( s, &lencode );
		if ( symbol < 0 )
		{
			return s->err ? s->err : INF_E_CODES;
		}
		if ( symbol < 16 )
		{
			lengths[index++] = (int16_t)symbol;
			continue;
		}
		len = 0;
		if ( symbol == 16 )  // repeat the previous length 3-6 times
		{
			if ( index == 0 )
			{
				return INF_E_CODES;
			}
			len = lengths[index - 1];
			symbol = 3 + inf_bits( s, 2 );
		}
		else if ( symbol == 17 )  // 3-10 zeros
		{
			symbol = 3 + inf_bits( s, 3 );
		}
		else  // 11-138 zeros
		{
			symbol = 11 + inf_bits( s, 7 );
		}
		if ( s->err )
		{
			return s->err;
		}
		if ( index + symbol > nlen + ndist )
		{
			return INF_E_CODES;
		}
		while ( symbol-- )
		{
			lengths[index++] = (int16_t)len;
		}
	}
	if ( lengths[256] == 0 )  // no end-of-block code: the block could never end
	{
		return INF_E_CODES;
	}

	// an incomplete code is allowed only when it has a single code
	err = inf_construct( &lencode, lengths, nlen );
	if ( err < 0 || ( err > 0 && nlen - lencode.count[0] != 1 ) )
	{
		return INF_E_CODES;
	}
	err = inf_construct( &distcode, lengths + nlen, ndist );
	if ( err < 0 || ( err > 0 && ndist - distcode.count[0] != 1 ) )
	{
		return INF_E_CODES;
	}
	return inf_codes( s, &lencode, &distcode );
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

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
				 size_t *produced )
{
	inf_state s;
	int last, type, err = 0;

	memset( &s, 0, sizeof s );
	s.in = in;
	s.in_len = in_len;
	s.out = out;
	s.out_len = out_len;
	do
	{
		last = inf_bits( &s, 1 );
		type = inf_bits( &s, 2 );
		if ( s.err )
		{
			err = s.err;
			break;
		}
		if ( type == 0 )
		{
			err = inf_stored( &s );
		}
		else if ( type == 1 )
		{
			err = inf_fixed( &s );
		}
		else if ( type == 2 )
		{
			err = inf_dynamic( &s );
		}
		else
		{
			err = INF_E_TYPE;
		}
	} while ( !err && !last );

	if ( produced )
	{
		*produced = s.out_pos;
	}
	return err;
}

/**
  * @brief  CRC-32 (IEEE 802.3, as used by zip), continuing from crc.
  *
  * @param  crc 0 for a new checksum, or the result of the previous call.
  * @param  p The data.
  * @param  n Bytes in p.
  *
  * @retval The updated CRC.
  */
uint32_t inf_crc32( uint32_t crc, const uint8_t *p, size_t n )
{
	uint32_t c;
	int i, k;

	if ( !inf_crc_ready )
	{
		for ( i = 0; i < 256; i++ )
		{
			c = (uint32_t)i;
			for ( k = 0; k < 8; k++ )
			{
				c = ( c & 1 ) ? 0xEDB88320u ^ ( c >> 1 ) : c >> 1;
			}
			inf_crc_table[i] = c;
		}
		inf_crc_ready = 1;
	}
	crc = ~crc;
	while ( n-- )
	{
		crc = inf_crc_table[( crc ^ *p++ ) & 0xFF] ^ ( crc >> 8 );
	}
	return ~crc;
}
