//  Description:
//
//		v30.c -- NEC V30 / V50 CPU core (8086 + 80186 + NEC extensions).
//
//		A straightforward interpreter: one big switch on the opcode, with the
//		ModR/M decoder and the ALU / shift / string helpers shared between
//		the byte and word forms. Flags are kept in their architectural bits
//		(no lazy evaluation): the disk engine is not fast enough to need it.
//
//		Timing is approximate (a few clocks per register operation, more for
//		memory operands, multiply and divide); nothing in the Darwin's disk
//		engine depends on exact instruction timing, only on the timer unit.
//
//		Differences from an Intel 8086 that matter here: 0F is NEC's
//		extended-instruction prefix (not POP CS), 64 / 65 are the REPNC /
//		REPC prefixes, AAM / AAD always use base 10, shift counts are taken
//		modulo 32 as on the 80186.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "v30.h"
#include "darwin.h"

#include <stdio.h>
#include <string.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define V30_LIN( seg, off ) ( ( ( (uint32_t)( seg ) << 4 ) + (uint16_t)( off ) ) & 0xFFFFFu )

#define AX c->r[V30_AX]
#define CX c->r[V30_CX]
#define DX c->r[V30_DX]
#define BX c->r[V30_BX]
#define SP c->r[V30_SP]
#define BP c->r[V30_BP]
#define SI c->r[V30_SI]
#define DI c->r[V30_DI]

#define FLAG( f ) ( ( c->flags & ( f ) ) != 0 )

// the ALU operations in the order the 00-3F opcodes and group 1 encode them
enum { V30_ADD = 0, V30_OR, V30_ADC, V30_SBB, V30_AND, V30_SUB, V30_XOR, V30_CMP };

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static uint8_t v30_parity[256];  // V30_PF for an even number of set bits
static int     v30_tables_ready;

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void v30_tables( void );
static uint8_t rd8( v30_cpu *c, int seg, uint16_t off );
static uint16_t rd16( v30_cpu *c, int seg, uint16_t off );
static void wr8( v30_cpu *c, int seg, uint16_t off, uint8_t v );
static void wr16( v30_cpu *c, int seg, uint16_t off, uint16_t v );
static uint8_t fetch8( v30_cpu *c );
static uint16_t fetch16( v30_cpu *c );
static void push( v30_cpu *c, uint16_t v );
static uint16_t pop( v30_cpu *c );
static uint8_t get_reg8( v30_cpu *c, int n );
static void set_reg8( v30_cpu *c, int n, uint8_t v );
static int seg_of( v30_cpu *c, int def );
static void modrm( v30_cpu *c, uint8_t m );
static uint8_t get_rm8( v30_cpu *c );
static uint16_t get_rm16( v30_cpu *c );
static void set_rm8( v30_cpu *c, uint8_t v );
static void set_rm16( v30_cpu *c, uint16_t v );
static void set_szp( v30_cpu *c, uint32_t r, int w );
static void set_flag( v30_cpu *c, uint16_t f, int on );
static uint32_t alu( v30_cpu *c, int op, uint32_t a, uint32_t b, int w );
static uint32_t inc_dec( v30_cpu *c, uint32_t a, int dec, int w );
static uint32_t shift( v30_cpu *c, int op, uint32_t v, int count, int w );
static void interrupt( v30_cpu *c, int n );
static void string_op( v30_cpu *c, uint8_t op );
static int cond( v30_cpu *c, int cc );
static int nec_0f( v30_cpu *c );
static int step( v30_cpu *c );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Build the parity table once.
  *
  * @retval None.
  */
static void v30_tables( void )
{
	int i, b, n;

	for ( i = 0; i < 256; i++ )
	{
		for ( b = n = 0; b < 8; b++ )
		{
			n += ( i >> b ) & 1;
		}
		v30_parity[i] = ( n & 1 ) ? 0 : V30_PF;
	}
	v30_tables_ready = 1;
}

/**
  * @brief  Read a byte at seg:off.
  *
  * @param  c The CPU.
  * @param  seg Segment register number.
  * @param  off Offset.
  *
  * @retval The byte.
  */
static uint8_t rd8( v30_cpu *c, int seg, uint16_t off ) { return c->mem[V30_LIN( c->sreg[seg], off )]; }

/**
  * @brief  Read a little-endian word at seg:off (the offset wraps in the segment).
  *
  * @param  c The CPU.
  * @param  seg Segment register number.
  * @param  off Offset.
  *
  * @retval The word.
  */
static uint16_t rd16( v30_cpu *c, int seg, uint16_t off )
{
	return (uint16_t)( c->mem[V30_LIN( c->sreg[seg], off )] |
					   ( c->mem[V30_LIN( c->sreg[seg], (uint16_t)( off + 1 ) )] << 8 ) );
}

/**
  * @brief  Write a byte at seg:off.
  *
  * @param  c The CPU.
  * @param  seg Segment register number.
  * @param  off Offset.
  * @param  v The byte.
  *
  * @retval None.
  */
static void wr8( v30_cpu *c, int seg, uint16_t off, uint8_t v ) { c->mem[V30_LIN( c->sreg[seg], off )] = v; }

/**
  * @brief  Write a little-endian word at seg:off.
  *
  * @param  c The CPU.
  * @param  seg Segment register number.
  * @param  off Offset.
  * @param  v The word.
  *
  * @retval None.
  */
static void wr16( v30_cpu *c, int seg, uint16_t off, uint16_t v )
{
	c->mem[V30_LIN( c->sreg[seg], off )] = (uint8_t)v;
	c->mem[V30_LIN( c->sreg[seg], (uint16_t)( off + 1 ) )] = (uint8_t)( v >> 8 );
}

/**
  * @brief  Fetch the next instruction byte.
  *
  * @param  c The CPU.
  *
  * @retval The byte.
  */
static uint8_t fetch8( v30_cpu *c )
{
	uint8_t v = c->mem[V30_LIN( c->sreg[V30_CS], c->ip )];

	c->ip++;
	return v;
}

/**
  * @brief  Fetch the next instruction word.
  *
  * @param  c The CPU.
  *
  * @retval The word.
  */
static uint16_t fetch16( v30_cpu *c )
{
	uint16_t lo = fetch8( c );

	return (uint16_t)( lo | ( fetch8( c ) << 8 ) );
}

/**
  * @brief  Push a word.
  *
  * @param  c The CPU.
  * @param  v The word.
  *
  * @retval None.
  */
static void push( v30_cpu *c, uint16_t v )
{
	SP -= 2;
	wr16( c, V30_SS, SP, v );
}

/**
  * @brief  Pop a word.
  *
  * @param  c The CPU.
  *
  * @retval The word.
  */
static uint16_t pop( v30_cpu *c )
{
	uint16_t v = rd16( c, V30_SS, SP );

	SP += 2;
	return v;
}

/**
  * @brief  Read an 8-bit register (AL CL DL BL AH CH DH BH).
  *
  * @param  c The CPU.
  * @param  n Register number 0-7.
  *
  * @retval The value.
  */
static uint8_t get_reg8( v30_cpu *c, int n )
{
	return n < 4 ? (uint8_t)c->r[n] : (uint8_t)( c->r[n - 4] >> 8 );
}

/**
  * @brief  Write an 8-bit register.
  *
  * @param  c The CPU.
  * @param  n Register number 0-7.
  * @param  v The value.
  *
  * @retval None.
  */
static void set_reg8( v30_cpu *c, int n, uint8_t v )
{
	if ( n < 4 )
	{
		c->r[n] = (uint16_t)( ( c->r[n] & 0xFF00 ) | v );
	}
	else
	{
		c->r[n - 4] = (uint16_t)( ( c->r[n - 4] & 0x00FF ) | ( v << 8 ) );
	}
}

/**
  * @brief  The segment a memory operand uses: the override, or the default.
  *
  * @param  c The CPU.
  * @param  def Default segment.
  *
  * @retval Segment register number.
  */
static int seg_of( v30_cpu *c, int def ) { return c->seg_ovr >= 0 ? c->seg_ovr : def; }

/**
  * @brief  Decode a ModR/M byte's r/m operand (fetching any displacement).
  *
  * @param  c The CPU; ea_reg, ea_seg, ea_off receive the operand.
  * @param  m The ModR/M byte.
  *
  * @retval None.
  */
static void modrm( v30_cpu *c, uint8_t m )
{
	int mod = m >> 6, rm = m & 7, seg = V30_DS;
	uint16_t off = 0;

	if ( mod == 3 )
	{
		c->ea_reg = rm;
		return;
	}
	c->ea_reg = -1;
	switch ( rm )
	{
	case 0: off = (uint16_t)( BX + SI ); break;
	case 1: off = (uint16_t)( BX + DI ); break;
	case 2: off = (uint16_t)( BP + SI ); seg = V30_SS; break;
	case 3: off = (uint16_t)( BP + DI ); seg = V30_SS; break;
	case 4: off = SI; break;
	case 5: off = DI; break;
	case 6:
		if ( mod == 0 )
		{
			off = fetch16( c );
		}
		else
		{
			off = BP;
			seg = V30_SS;
		}
		break;
	default: off = BX; break;
	}
	if ( mod == 1 )
	{
		off = (uint16_t)( off + (int8_t)fetch8( c ) );
	}
	else if ( mod == 2 )
	{
		off = (uint16_t)( off + fetch16( c ) );
	}
	c->ea_seg = seg_of( c, seg );
	c->ea_off = off;
	c->cycles += 5;  // address calculation and the memory access
}

/**
  * @brief  Read the decoded r/m operand, byte.
  *
  * @param  c The CPU.
  *
  * @retval The byte.
  */
static uint8_t get_rm8( v30_cpu *c )
{
	return c->ea_reg >= 0 ? get_reg8( c, c->ea_reg ) : rd8( c, c->ea_seg, c->ea_off );
}

/**
  * @brief  Read the decoded r/m operand, word.
  *
  * @param  c The CPU.
  *
  * @retval The word.
  */
static uint16_t get_rm16( v30_cpu *c )
{
	return c->ea_reg >= 0 ? c->r[c->ea_reg] : rd16( c, c->ea_seg, c->ea_off );
}

/**
  * @brief  Write the decoded r/m operand, byte.
  *
  * @param  c The CPU.
  * @param  v The byte.
  *
  * @retval None.
  */
static void set_rm8( v30_cpu *c, uint8_t v )
{
	if ( c->ea_reg >= 0 )
	{
		set_reg8( c, c->ea_reg, v );
	}
	else
	{
		wr8( c, c->ea_seg, c->ea_off, v );
	}
}

/**
  * @brief  Write the decoded r/m operand, word.
  *
  * @param  c The CPU.
  * @param  v The word.
  *
  * @retval None.
  */
static void set_rm16( v30_cpu *c, uint16_t v )
{
	if ( c->ea_reg >= 0 )
	{
		c->r[c->ea_reg] = v;
	}
	else
	{
		wr16( c, c->ea_seg, c->ea_off, v );
	}
}

/**
  * @brief  Set SF, ZF and PF from a result.
  *
  * @param  c The CPU.
  * @param  r The result.
  * @param  w 1 for a word, 0 for a byte.
  *
  * @retval None.
  */
static void set_szp( v30_cpu *c, uint32_t r, int w )
{
	uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u;

	c->flags &= (uint16_t)~( V30_SF | V30_ZF | V30_PF );
	if ( ( r & mask ) == 0 )
	{
		c->flags |= V30_ZF;
	}
	if ( r & sign )
	{
		c->flags |= V30_SF;
	}
	c->flags |= v30_parity[r & 0xFF];
}

/**
  * @brief  Set or clear one flag.
  *
  * @param  c The CPU.
  * @param  f The flag bit.
  * @param  on Non-zero to set.
  *
  * @retval None.
  */
static void set_flag( v30_cpu *c, uint16_t f, int on )
{
	if ( on )
	{
		c->flags |= f;
	}
	else
	{
		c->flags &= (uint16_t)~f;
	}
}

/**
  * @brief  The eight ALU operations, with all their flags.
  *
  * @param  c The CPU.
  * @param  op V30_ADD ... V30_CMP.
  * @param  a First operand (destination).
  * @param  b Second operand.
  * @param  w 1 for a word, 0 for a byte.
  *
  * @retval The result (for CMP, the difference, which the caller discards).
  */
static uint32_t alu( v30_cpu *c, int op, uint32_t a, uint32_t b, int w )
{
	uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u, r = 0, cf = FLAG( V30_CF );

	a &= mask;
	b &= mask;
	switch ( op )
	{
	case V30_ADD:
	case V30_ADC:
		r = a + b + ( op == V30_ADC ? cf : 0 );
		set_flag( c, V30_CF, r > mask );
		set_flag( c, V30_OF, ( ( r ^ a ) & ( r ^ b ) & sign ) != 0 );
		set_flag( c, V30_AF, ( ( a ^ b ^ r ) & 0x10 ) != 0 );
		break;
	case V30_SUB:
	case V30_SBB:
	case V30_CMP:
		r = a - b - ( op == V30_SBB ? cf : 0 );
		set_flag( c, V30_CF, ( r & ( mask + 1 ) ) != 0 );  // a borrow wraps above mask
		set_flag( c, V30_OF, ( ( a ^ b ) & ( a ^ r ) & sign ) != 0 );
		set_flag( c, V30_AF, ( ( a ^ b ^ r ) & 0x10 ) != 0 );
		break;
	case V30_OR:
		r = a | b;
		c->flags &= (uint16_t)~( V30_CF | V30_OF | V30_AF );
		break;
	case V30_AND:
		r = a & b;
		c->flags &= (uint16_t)~( V30_CF | V30_OF | V30_AF );
		break;
	default:  // XOR
		r = a ^ b;
		c->flags &= (uint16_t)~( V30_CF | V30_OF | V30_AF );
		break;
	}
	r &= mask;
	set_szp( c, r, w );
	return r;
}

/**
  * @brief  INC / DEC: like ADD / SUB 1, but CF is left alone.
  *
  * @param  c The CPU.
  * @param  a The operand.
  * @param  dec 1 for DEC.
  * @param  w 1 for a word, 0 for a byte.
  *
  * @retval The result.
  */
static uint32_t inc_dec( v30_cpu *c, uint32_t a, int dec, int w )
{
	uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u, r;

	a &= mask;
	r = ( dec ? a - 1 : a + 1 ) & mask;
	set_flag( c, V30_OF, dec ? a == sign : r == sign );
	set_flag( c, V30_AF, ( ( a ^ 1 ^ r ) & 0x10 ) != 0 );
	set_szp( c, r, w );
	return r;
}

/**
  * @brief  Group 2: rotates and shifts.
  *
  * @param  c The CPU.
  * @param  op 0 ROL, 1 ROR, 2 RCL, 3 RCR, 4 SHL, 5 SHR, 6 SHL, 7 SAR.
  * @param  v The operand.
  * @param  count Shift count.
  * @param  w 1 for a word, 0 for a byte.
  *
  * @retval The result.
  */
static uint32_t shift( v30_cpu *c, int op, uint32_t v, int count, int w )
{
	uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u, cf, top;
	int bits = w ? 16 : 8, i;

	count &= 0x1F;
	v &= mask;
	if ( count == 0 )
	{
		return v;
	}
	c->cycles += (uint64_t)count;
	switch ( op )
	{
	case 0:  // ROL
		for ( i = 0; i < count; i++ )
		{
			cf = ( v >> ( bits - 1 ) ) & 1;
			v = ( ( v << 1 ) | cf ) & mask;
		}
		set_flag( c, V30_CF, v & 1 );
		set_flag( c, V30_OF, ( ( v >> ( bits - 1 ) ) ^ v ) & 1 );
		return v;
	case 1:  // ROR
		for ( i = 0; i < count; i++ )
		{
			cf = v & 1;
			v = ( v >> 1 ) | ( cf << ( bits - 1 ) );
		}
		set_flag( c, V30_CF, ( v & sign ) != 0 );
		set_flag( c, V30_OF, ( ( v >> ( bits - 1 ) ) ^ ( v >> ( bits - 2 ) ) ) & 1 );
		return v;
	case 2:  // RCL
		for ( i = 0; i < count; i++ )
		{
			cf = ( v >> ( bits - 1 ) ) & 1;
			v = ( ( v << 1 ) | (uint32_t)FLAG( V30_CF ) ) & mask;
			set_flag( c, V30_CF, (int)cf );
		}
		set_flag( c, V30_OF, ( ( v >> ( bits - 1 ) ) & 1 ) ^ (uint32_t)FLAG( V30_CF ) );
		return v;
	case 3:  // RCR
		set_flag( c, V30_OF, ( ( v >> ( bits - 1 ) ) & 1 ) ^ (uint32_t)FLAG( V30_CF ) );
		for ( i = 0; i < count; i++ )
		{
			cf = v & 1;
			v = ( v >> 1 ) | ( (uint32_t)FLAG( V30_CF ) << ( bits - 1 ) );
			set_flag( c, V30_CF, (int)cf );
		}
		return v;
	case 4:
	case 6:  // SHL / SAL
		cf = count <= bits ? ( v >> ( bits - count ) ) & 1 : 0;
		v = count < 32 ? ( v << count ) & mask : 0;
		set_flag( c, V30_CF, (int)cf );
		set_flag( c, V30_OF, ( ( v >> ( bits - 1 ) ) & 1 ) != cf );
		set_szp( c, v, w );
		return v;
	case 5:  // SHR
		top = v & sign;
		cf = count <= bits ? ( v >> ( count - 1 ) ) & 1 : 0;
		v = count < bits ? v >> count : 0;
		set_flag( c, V30_CF, (int)cf );
		set_flag( c, V30_OF, top != 0 );
		set_szp( c, v, w );
		return v;
	default:  // SAR
		for ( i = 0; i < count; i++ )
		{
			cf = v & 1;
			v = ( v >> 1 ) | ( v & sign );
		}
		set_flag( c, V30_CF, (int)cf );
		c->flags &= (uint16_t)~V30_OF;
		set_szp( c, v, w );
		return v;
	}
}

/**
  * @brief  Take an interrupt: push FLAGS, CS, IP and jump through the vector.
  *
  * @param  c The CPU.
  * @param  n Vector number.
  *
  * @retval None.
  */
static void interrupt( v30_cpu *c, int n )
{
	uint32_t vec = (uint32_t)( n & 0xFF ) * 4;

	if ( n == 0 || n == 1 || n == 5 || n == 6 || n == 7 )  // CPU exceptions (no hardware interrupt uses them here)
	{
		dw_log( "%10.6f v30: exception %d at %04X:%04X\n", (double)dw_cycles / DW_CPU_HZ, n, c->prev_cs, c->prev_ip );
	}
	push( c, c->flags );
	c->flags &= (uint16_t)~( V30_IF | V30_TF );
	push( c, c->sreg[V30_CS] );
	push( c, c->ip );
	c->ip = (uint16_t)( c->mem[vec] | ( c->mem[vec + 1] << 8 ) );
	c->sreg[V30_CS] = (uint16_t)( c->mem[vec + 2] | ( c->mem[vec + 3] << 8 ) );
	c->halted = 0;
	c->cycles += 40;
}

/**
  * @brief  One string instruction, with any REP prefix.
  *
  * @param  c The CPU.
  * @param  op The opcode (A4-A7, AA-AF, 6C-6F).
  *
  * @retval None.
  */
static void string_op( v30_cpu *c, uint8_t op )
{
	int w = op & 1, size = w ? 2 : 1, src = seg_of( c, V30_DS );
	int16_t d = FLAG( V30_DF ) ? (int16_t)-size : (int16_t)size;
	int cmp = op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF;
	uint32_t a, b;

	for ( ;; )
	{
		if ( c->rep && CX == 0 )
		{
			break;
		}
		switch ( op )
		{
		case 0xA4:
			wr8( c, V30_ES, DI, rd8( c, src, SI ) );
			SI += d;
			DI += d;
			break;
		case 0xA5:
			wr16( c, V30_ES, DI, rd16( c, src, SI ) );
			SI += d;
			DI += d;
			break;
		case 0xA6:
		case 0xA7:
			a = w ? rd16( c, src, SI ) : rd8( c, src, SI );
			b = w ? rd16( c, V30_ES, DI ) : rd8( c, V30_ES, DI );
			alu( c, V30_CMP, a, b, w );
			SI += d;
			DI += d;
			break;
		case 0xAA:
			wr8( c, V30_ES, DI, (uint8_t)AX );
			DI += d;
			break;
		case 0xAB:
			wr16( c, V30_ES, DI, AX );
			DI += d;
			break;
		case 0xAC:
			set_reg8( c, 0, rd8( c, src, SI ) );
			SI += d;
			break;
		case 0xAD:
			AX = rd16( c, src, SI );
			SI += d;
			break;
		case 0xAE:
		case 0xAF:
			b = w ? rd16( c, V30_ES, DI ) : rd8( c, V30_ES, DI );
			alu( c, V30_CMP, AX, b, w );
			DI += d;
			break;
		case 0x6C:
			wr8( c, V30_ES, DI, c->in8 ? c->in8( DX ) : 0xFF );
			DI += d;
			break;
		case 0x6D:
			wr16( c, V30_ES, DI, c->in16 ? c->in16( DX ) : 0xFFFF );
			DI += d;
			break;
		case 0x6E:
			if ( c->out8 )
			{
				c->out8( DX, rd8( c, src, SI ) );
			}
			SI += d;
			break;
		default:  // 6F
			if ( c->out16 )
			{
				c->out16( DX, rd16( c, src, SI ) );
			}
			SI += d;
			break;
		}
		c->cycles += 8;
		if ( !c->rep )
		{
			break;
		}
		CX--;
		if ( cmp )
		{
			if ( ( c->rep == 0xF3 && !FLAG( V30_ZF ) ) || ( c->rep == 0xF2 && FLAG( V30_ZF ) ) ||
				 ( c->rep == 0x65 && !FLAG( V30_CF ) ) || ( c->rep == 0x64 && FLAG( V30_CF ) ) )
			{
				break;
			}
		}
	}
}

/**
  * @brief  Evaluate a Jcc condition (the low nibble of 70-7F).
  *
  * @param  c The CPU.
  * @param  cc Condition code 0-15.
  *
  * @retval 1 if the condition holds.
  */
static int cond( v30_cpu *c, int cc )
{
	int r;

	switch ( cc >> 1 )
	{
	case 0: r = FLAG( V30_OF ); break;
	case 1: r = FLAG( V30_CF ); break;
	case 2: r = FLAG( V30_ZF ); break;
	case 3: r = FLAG( V30_CF ) || FLAG( V30_ZF ); break;
	case 4: r = FLAG( V30_SF ); break;
	case 5: r = FLAG( V30_PF ); break;
	case 6: r = FLAG( V30_SF ) != FLAG( V30_OF ); break;
	default: r = FLAG( V30_ZF ) || ( FLAG( V30_SF ) != FLAG( V30_OF ) ); break;
	}
	return ( cc & 1 ) ? !r : r;
}

/**
  * @brief  NEC extended instructions (0F prefix).
  *
  * @param  c The CPU (IP after the 0F byte).
  *
  * @retval 0 handled, -1 unknown.
  */
static int nec_0f( v30_cpu *c )
{
	uint8_t op = fetch8( c ), m, v8, a8, b8, digit;
	uint16_t v16;
	int bit, i, n, carry, w, kind;
	uint32_t lin;

	switch ( op )
	{
	case 0x10: case 0x11: case 0x12: case 0x13:  // TEST1 / CLR1 / SET1 / NOT1, bit in CL
	case 0x14: case 0x15: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1A: case 0x1B:  // ... bit as an immediate
	case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		m = fetch8( c );
		modrm( c, m );
		w = op & 1;
		kind = ( op >> 1 ) & 3;
		bit = ( op & 8 ) ? fetch8( c ) : (uint8_t)CX;
		bit &= w ? 15 : 7;
		v16 = w ? get_rm16( c ) : get_rm8( c );
		if ( kind == 0 )
		{
			set_flag( c, V30_ZF, !( ( v16 >> bit ) & 1 ) );
			c->flags &= (uint16_t)~( V30_CF | V30_OF );
			return 0;
		}
		if ( kind == 1 )
		{
			v16 &= (uint16_t)~( 1u << bit );
		}
		else if ( kind == 2 )
		{
			v16 |= (uint16_t)( 1u << bit );
		}
		else
		{
			v16 ^= (uint16_t)( 1u << bit );
		}
		if ( w )
		{
			set_rm16( c, v16 );
		}
		else
		{
			set_rm8( c, (uint8_t)v16 );
		}
		return 0;
	case 0x20: case 0x22: case 0x26:  // ADD4S / SUB4S / CMP4S: packed BCD strings, CL digits
		n = ( (uint8_t)CX + 1 ) / 2;
		carry = 0;
		set_flag( c, V30_ZF, 1 );
		for ( i = 0; i < n; i++ )
		{
			lin = V30_LIN( c->sreg[seg_of( c, V30_DS )], (uint16_t)( SI + i ) );
			a8 = c->mem[lin];
			b8 = c->mem[V30_LIN( c->sreg[V30_ES], (uint16_t)( DI + i ) )];
			v8 = 0;
			for ( bit = 0; bit < 8; bit += 4 )
			{
				int x = ( b8 >> bit ) & 15, y = ( a8 >> bit ) & 15, s;

				if ( op == 0x20 )
				{
					s = x + y + carry;
					carry = s > 9;
					if ( carry )
					{
						s -= 10;
					}
				}
				else
				{
					s = x - y - carry;
					carry = s < 0;
					if ( carry )
					{
						s += 10;
					}
				}
				digit = (uint8_t)s;
				v8 |= (uint8_t)( digit << bit );
			}
			if ( v8 )
			{
				set_flag( c, V30_ZF, 0 );
			}
			if ( op != 0x26 )
			{
				c->mem[V30_LIN( c->sreg[V30_ES], (uint16_t)( DI + i ) )] = v8;
			}
		}
		set_flag( c, V30_CF, carry );
		c->cycles += (uint64_t)( 20 * n );
		return 0;
	case 0x28:  // ROL4: AL low nibble and the operand's nibbles rotate left
	case 0x2A:  // ROR4
		m = fetch8( c );
		modrm( c, m );
		v8 = get_rm8( c );
		a8 = (uint8_t)AX;
		if ( op == 0x28 )
		{
			set_rm8( c, (uint8_t)( ( v8 << 4 ) | ( a8 & 15 ) ) );
			set_reg8( c, 0, (uint8_t)( ( a8 & 0xF0 ) | ( v8 >> 4 ) ) );
		}
		else
		{
			set_rm8( c, (uint8_t)( ( ( a8 & 15 ) << 4 ) | ( v8 >> 4 ) ) );
			set_reg8( c, 0, (uint8_t)( ( a8 & 0xF0 ) | ( v8 & 15 ) ) );
		}
		return 0;
	case 0x31: case 0x39:  // INS reg8, reg8 / imm4: insert AX's low bits as a bit field at ES:DI
	case 0x33: case 0x3B:  // EXT reg8, reg8 / imm4: extract a bit field at DS:SI into AX
		m = fetch8( c );
		n = ( ( op & 8 ) ? fetch8( c ) : get_reg8( c, m & 7 ) ) & 15;  // field length - 1
		bit = get_reg8( c, ( m >> 3 ) & 7 ) & 15;                     // bit offset
		n += 1;
		if ( op == 0x31 || op == 0x39 )
		{
			uint32_t word, fmask;

			lin = V30_LIN( c->sreg[V30_ES], DI );
			word = c->mem[lin] | ( c->mem[( lin + 1 ) & 0xFFFFF] << 8 ) |
				   ( (uint32_t)c->mem[( lin + 2 ) & 0xFFFFF] << 16 );
			fmask = ( ( 1u << n ) - 1 ) << bit;
			word = ( word & ~fmask ) | ( ( (uint32_t)AX << bit ) & fmask );
			c->mem[lin] = (uint8_t)word;
			c->mem[( lin + 1 ) & 0xFFFFF] = (uint8_t)( word >> 8 );
			c->mem[( lin + 2 ) & 0xFFFFF] = (uint8_t)( word >> 16 );
			bit += n;
			DI = (uint16_t)( DI + ( bit >> 3 & ~1 ) );
		}
		else
		{
			uint32_t word;

			lin = V30_LIN( c->sreg[seg_of( c, V30_DS )], SI );
			word = c->mem[lin] | ( c->mem[( lin + 1 ) & 0xFFFFF] << 8 ) |
				   ( (uint32_t)c->mem[( lin + 2 ) & 0xFFFFF] << 16 );
			AX = (uint16_t)( ( word >> bit ) & ( ( 1u << n ) - 1 ) );
			bit += n;
			SI = (uint16_t)( SI + ( bit >> 3 & ~1 ) );
		}
		set_reg8( c, ( m >> 3 ) & 7, (uint8_t)( bit & 15 ) );
		return 0;
	default:
		return -1;
	}
}

/**
  * @brief  Execute one instruction (with its prefixes).
  *
  * @param  c The CPU.
  *
  * @retval 0 normally, -1 an undefined opcode (logged, skipped).
  */
static int step( v30_cpu *c )
{
	uint8_t op, m;
	uint16_t v16, a16, off, seg;
	uint32_t r32, lin;
	int w, reg, n, i;
	int32_t s32;

	c->seg_ovr = -1;
	c->rep = 0;
	c->prev_ip = c->ip;
	c->prev_cs = c->sreg[V30_CS];
	for ( ;; )  // prefixes
	{
		op = fetch8( c );
		if ( op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E )
		{
			c->seg_ovr = ( op >> 3 ) & 3;
		}
		else if ( op == 0xF2 || op == 0xF3 || op == 0x64 || op == 0x65 )
		{
			c->rep = op;
		}
		else if ( op != 0xF0 )  // LOCK has no effect here
		{
			break;
		}
		c->cycles += 2;
	}
	c->cycles += 3;

	// 00-3F: the eight ALU operations in six forms each, plus the odd ones
	if ( op < 0x40 && ( op & 7 ) < 6 )
	{
		int alu_op = op >> 3;

		w = op & 1;
		switch ( op & 7 )
		{
		case 0:  // r/m8, r8
		case 1:  // r/m16, r16
			m = fetch8( c );
			modrm( c, m );
			reg = ( m >> 3 ) & 7;
			if ( w )
			{
				r32 = alu( c, alu_op, get_rm16( c ), c->r[reg], 1 );
				if ( alu_op != V30_CMP )
				{
					set_rm16( c, (uint16_t)r32 );
				}
			}
			else
			{
				r32 = alu( c, alu_op, get_rm8( c ), get_reg8( c, reg ), 0 );
				if ( alu_op != V30_CMP )
				{
					set_rm8( c, (uint8_t)r32 );
				}
			}
			return 0;
		case 2:  // r8, r/m8
		case 3:  // r16, r/m16
			m = fetch8( c );
			modrm( c, m );
			reg = ( m >> 3 ) & 7;
			if ( w )
			{
				r32 = alu( c, alu_op, c->r[reg], get_rm16( c ), 1 );
				if ( alu_op != V30_CMP )
				{
					c->r[reg] = (uint16_t)r32;
				}
			}
			else
			{
				r32 = alu( c, alu_op, get_reg8( c, reg ), get_rm8( c ), 0 );
				if ( alu_op != V30_CMP )
				{
					set_reg8( c, reg, (uint8_t)r32 );
				}
			}
			return 0;
		case 4:  // AL, imm8
			r32 = alu( c, alu_op, (uint8_t)AX, fetch8( c ), 0 );
			if ( alu_op != V30_CMP )
			{
				set_reg8( c, 0, (uint8_t)r32 );
			}
			return 0;
		default:  // AX, imm16
			r32 = alu( c, alu_op, AX, fetch16( c ), 1 );
			if ( alu_op != V30_CMP )
			{
				AX = (uint16_t)r32;
			}
			return 0;
		}
	}

	switch ( op )
	{
	case 0x06: case 0x0E: case 0x16: case 0x1E:  // PUSH sreg
		push( c, c->sreg[( op >> 3 ) & 3] );
		return 0;
	case 0x07: case 0x17: case 0x1F:  // POP sreg
		c->sreg[( op >> 3 ) & 3] = pop( c );
		if ( op == 0x17 )
		{
			c->inhibit = 1;
		}
		return 0;
	case 0x0F:
		if ( nec_0f( c ) != 0 )
		{
			return -1;
		}
		return 0;
	case 0x27:  // DAA
	case 0x2F:  // DAS
	{
		uint8_t al = (uint8_t)AX, old = al;
		int old_cf = FLAG( V30_CF );

		c->flags &= (uint16_t)~V30_CF;
		if ( ( al & 15 ) > 9 || FLAG( V30_AF ) )
		{
			al = (uint8_t)( op == 0x27 ? al + 6 : al - 6 );
			set_flag( c, V30_CF, old_cf || ( op == 0x27 ? old > 0xF9 : old < 6 ) );
			c->flags |= V30_AF;
		}
		else
		{
			c->flags &= (uint16_t)~V30_AF;
		}
		if ( old > 0x99 || old_cf )
		{
			al = (uint8_t)( op == 0x27 ? al + 0x60 : al - 0x60 );
			c->flags |= V30_CF;
		}
		set_reg8( c, 0, al );
		set_szp( c, al, 0 );
		return 0;
	}
	case 0x37:  // AAA
	case 0x3F:  // AAS
		if ( ( AX & 15 ) > 9 || FLAG( V30_AF ) )
		{
			if ( op == 0x37 )
			{
				AX = (uint16_t)( AX + 0x106 );
			}
			else
			{
				AX = (uint16_t)( AX - 6 );
				AX = (uint16_t)( AX - 0x100 );
			}
			c->flags |= V30_AF | V30_CF;
		}
		else
		{
			c->flags &= (uint16_t)~( V30_AF | V30_CF );
		}
		AX &= 0xFF0F;
		return 0;
	default:
		break;
	}

	if ( op >= 0x40 && op < 0x50 )  // INC / DEC r16
	{
		reg = op & 7;
		c->r[reg] = (uint16_t)inc_dec( c, c->r[reg], op >= 0x48, 1 );
		return 0;
	}
	if ( op >= 0x50 && op < 0x58 )  // PUSH r16
	{
		v16 = c->r[op & 7];  // PUSH SP pushes the value before the decrement
		push( c, v16 );
		return 0;
	}
	if ( op >= 0x58 && op < 0x60 )  // POP r16
	{
		c->r[op & 7] = pop( c );
		return 0;
	}
	if ( op >= 0x70 && op < 0x80 )  // Jcc short
	{
		s32 = (int8_t)fetch8( c );
		if ( cond( c, op & 15 ) )
		{
			c->ip = (uint16_t)( c->ip + s32 );
			c->cycles += 8;
		}
		return 0;
	}
	if ( op >= 0x91 && op < 0x98 )  // XCHG AX, r16
	{
		v16 = AX;
		AX = c->r[op & 7];
		c->r[op & 7] = v16;
		return 0;
	}
	if ( op >= 0xB0 && op < 0xB8 )  // MOV r8, imm8
	{
		set_reg8( c, op & 7, fetch8( c ) );
		return 0;
	}
	if ( op >= 0xB8 && op < 0xC0 )  // MOV r16, imm16
	{
		c->r[op & 7] = fetch16( c );
		return 0;
	}
	if ( op >= 0xD8 && op < 0xE0 )  // ESC: no coprocessor, skip the operand
	{
		modrm( c, fetch8( c ) );
		return 0;
	}

	switch ( op )
	{
	case 0x60:  // PUSHA
		v16 = SP;
		for ( i = 0; i < 8; i++ )
		{
			push( c, i == V30_SP ? v16 : c->r[i] );
		}
		c->cycles += 30;
		return 0;
	case 0x61:  // POPA
		for ( i = 7; i >= 0; i-- )
		{
			v16 = pop( c );
			if ( i != V30_SP )
			{
				c->r[i] = v16;
			}
		}
		c->cycles += 30;
		return 0;
	case 0x62:  // BOUND r16, m16&16
		m = fetch8( c );
		modrm( c, m );
		reg = ( m >> 3 ) & 7;
		if ( (int16_t)c->r[reg] < (int16_t)rd16( c, c->ea_seg, c->ea_off ) ||
			 (int16_t)c->r[reg] > (int16_t)rd16( c, c->ea_seg, (uint16_t)( c->ea_off + 2 ) ) )
		{
			interrupt( c, 5 );
		}
		return 0;
	case 0x68:  // PUSH imm16
		push( c, fetch16( c ) );
		return 0;
	case 0x6A:  // PUSH imm8 (sign-extended)
		push( c, (uint16_t)(int16_t)(int8_t)fetch8( c ) );
		return 0;
	case 0x69:  // IMUL r16, r/m16, imm16
	case 0x6B:  // IMUL r16, r/m16, imm8
		m = fetch8( c );
		modrm( c, m );
		reg = ( m >> 3 ) & 7;
		s32 = (int16_t)get_rm16( c );
		s32 *= op == 0x69 ? (int16_t)fetch16( c ) : (int8_t)fetch8( c );
		c->r[reg] = (uint16_t)s32;
		set_flag( c, V30_CF, s32 != (int16_t)s32 );
		set_flag( c, V30_OF, s32 != (int16_t)s32 );
		c->cycles += 30;
		return 0;
	case 0x6C: case 0x6D: case 0x6E: case 0x6F:  // INS / OUTS
	case 0xA4: case 0xA5: case 0xA6: case 0xA7:  // MOVS / CMPS
	case 0xAA: case 0xAB: case 0xAC: case 0xAD:  // STOS / LODS
	case 0xAE: case 0xAF:                        // SCAS
		string_op( c, op );
		return 0;
	case 0x80:  // group 1 r/m8, imm8
	case 0x81:  // group 1 r/m16, imm16
	case 0x82:  // (alias of 80)
	case 0x83:  // group 1 r/m16, imm8 sign-extended
		m = fetch8( c );
		modrm( c, m );
		w = op & 1;
		n = ( m >> 3 ) & 7;
		if ( op == 0x81 )
		{
			a16 = fetch16( c );
		}
		else if ( op == 0x83 )
		{
			a16 = (uint16_t)(int16_t)(int8_t)fetch8( c );
		}
		else
		{
			a16 = fetch8( c );
		}
		if ( w )
		{
			r32 = alu( c, n, get_rm16( c ), a16, 1 );
			if ( n != V30_CMP )
			{
				set_rm16( c, (uint16_t)r32 );
			}
		}
		else
		{
			r32 = alu( c, n, get_rm8( c ), a16, 0 );
			if ( n != V30_CMP )
			{
				set_rm8( c, (uint8_t)r32 );
			}
		}
		return 0;
	case 0x84:  // TEST r/m8, r8
	case 0x85:  // TEST r/m16, r16
		m = fetch8( c );
		modrm( c, m );
		reg = ( m >> 3 ) & 7;
		if ( op & 1 )
		{
			alu( c, V30_AND, get_rm16( c ), c->r[reg], 1 );
		}
		else
		{
			alu( c, V30_AND, get_rm8( c ), get_reg8( c, reg ), 0 );
		}
		return 0;
	case 0x86:  // XCHG r/m8, r8
		m = fetch8( c );
		modrm( c, m );
		reg = ( m >> 3 ) & 7;
		v16 = get_rm8( c );
		set_rm8( c, get_reg8( c, reg ) );
		set_reg8( c, reg, (uint8_t)v16 );
		return 0;
	case 0x87:  // XCHG r/m16, r16
		m = fetch8( c );
		modrm( c, m );
		reg = ( m >> 3 ) & 7;
		v16 = get_rm16( c );
		set_rm16( c, c->r[reg] );
		c->r[reg] = v16;
		return 0;
	case 0x88:  // MOV r/m8, r8
		m = fetch8( c );
		modrm( c, m );
		set_rm8( c, get_reg8( c, ( m >> 3 ) & 7 ) );
		return 0;
	case 0x89:  // MOV r/m16, r16
		m = fetch8( c );
		modrm( c, m );
		set_rm16( c, c->r[( m >> 3 ) & 7] );
		return 0;
	case 0x8A:  // MOV r8, r/m8
		m = fetch8( c );
		modrm( c, m );
		set_reg8( c, ( m >> 3 ) & 7, get_rm8( c ) );
		return 0;
	case 0x8B:  // MOV r16, r/m16
		m = fetch8( c );
		modrm( c, m );
		c->r[( m >> 3 ) & 7] = get_rm16( c );
		return 0;
	case 0x8C:  // MOV r/m16, sreg
		m = fetch8( c );
		modrm( c, m );
		set_rm16( c, c->sreg[( m >> 3 ) & 3] );
		return 0;
	case 0x8D:  // LEA
		m = fetch8( c );
		modrm( c, m );
		if ( c->ea_reg < 0 )
		{
			c->r[( m >> 3 ) & 7] = c->ea_off;
		}
		return 0;
	case 0x8E:  // MOV sreg, r/m16
		m = fetch8( c );
		modrm( c, m );
		reg = ( m >> 3 ) & 3;
		if ( reg != V30_CS )
		{
			c->sreg[reg] = get_rm16( c );
		}
		if ( reg == V30_SS )
		{
			c->inhibit = 1;
		}
		return 0;
	case 0x8F:  // POP r/m16
		v16 = pop( c );
		m = fetch8( c );
		modrm( c, m );
		set_rm16( c, v16 );
		return 0;
	case 0x90:  // NOP
		return 0;
	case 0x98:  // CBW
		AX = (uint16_t)(int16_t)(int8_t)AX;
		return 0;
	case 0x99:  // CWD
		DX = ( AX & 0x8000 ) ? 0xFFFF : 0;
		return 0;
	case 0x9A:  // CALL far imm
		off = fetch16( c );
		seg = fetch16( c );
		push( c, c->sreg[V30_CS] );
		push( c, c->ip );
		c->ip = off;
		c->sreg[V30_CS] = seg;
		c->cycles += 20;
		return 0;
	case 0x9B:  // WAIT (POLL): no coprocessor
		return 0;
	case 0x9C:  // PUSHF
		push( c, (uint16_t)( c->flags | 0xF002 ) );
		return 0;
	case 0x9D:  // POPF
		c->flags = (uint16_t)( pop( c ) & 0x0FD5 );
		return 0;
	case 0x9E:  // SAHF
		c->flags = (uint16_t)( ( c->flags & 0xFF00 ) | ( ( AX >> 8 ) & 0xD5 ) );
		return 0;
	case 0x9F:  // LAHF
		set_reg8( c, 4, (uint8_t)( ( c->flags & 0xD5 ) | 0x02 ) );
		return 0;
	case 0xA0:  // MOV AL, moffs8
		off = fetch16( c );
		set_reg8( c, 0, rd8( c, seg_of( c, V30_DS ), off ) );
		c->cycles += 5;
		return 0;
	case 0xA1:  // MOV AX, moffs16
		off = fetch16( c );
		AX = rd16( c, seg_of( c, V30_DS ), off );
		c->cycles += 5;
		return 0;
	case 0xA2:  // MOV moffs8, AL
		off = fetch16( c );
		wr8( c, seg_of( c, V30_DS ), off, (uint8_t)AX );
		c->cycles += 5;
		return 0;
	case 0xA3:  // MOV moffs16, AX
		off = fetch16( c );
		wr16( c, seg_of( c, V30_DS ), off, AX );
		c->cycles += 5;
		return 0;
	case 0xA8:  // TEST AL, imm8
		alu( c, V30_AND, (uint8_t)AX, fetch8( c ), 0 );
		return 0;
	case 0xA9:  // TEST AX, imm16
		alu( c, V30_AND, AX, fetch16( c ), 1 );
		return 0;
	case 0xC0:  // group 2 r/m8, imm8
	case 0xC1:  // group 2 r/m16, imm8
	case 0xD0:  // group 2 r/m8, 1
	case 0xD1:  // group 2 r/m16, 1
	case 0xD2:  // group 2 r/m8, CL
	case 0xD3:  // group 2 r/m16, CL
		m = fetch8( c );
		modrm( c, m );
		w = op & 1;
		n = op >= 0xD2 ? (uint8_t)CX : op >= 0xD0 ? 1 : fetch8( c );
		if ( w )
		{
			set_rm16( c, (uint16_t)shift( c, ( m >> 3 ) & 7, get_rm16( c ), n, 1 ) );
		}
		else
		{
			set_rm8( c, (uint8_t)shift( c, ( m >> 3 ) & 7, get_rm8( c ), n, 0 ) );
		}
		return 0;
	case 0xC2:  // RET imm16
		a16 = fetch16( c );
		c->ip = pop( c );
		SP = (uint16_t)( SP + a16 );
		c->cycles += 12;
		return 0;
	case 0xC3:  // RET
		c->ip = pop( c );
		c->cycles += 10;
		return 0;
	case 0xC4:  // LES
	case 0xC5:  // LDS
		m = fetch8( c );
		modrm( c, m );
		c->r[( m >> 3 ) & 7] = rd16( c, c->ea_seg, c->ea_off );
		c->sreg[op == 0xC4 ? V30_ES : V30_DS] = rd16( c, c->ea_seg, (uint16_t)( c->ea_off + 2 ) );
		return 0;
	case 0xC6:  // MOV r/m8, imm8
		m = fetch8( c );
		modrm( c, m );
		set_rm8( c, fetch8( c ) );
		return 0;
	case 0xC7:  // MOV r/m16, imm16
		m = fetch8( c );
		modrm( c, m );
		set_rm16( c, fetch16( c ) );
		return 0;
	case 0xC8:  // ENTER imm16, imm8
		a16 = fetch16( c );
		n = fetch8( c ) & 0x1F;
		push( c, BP );
		v16 = SP;
		if ( n > 0 )
		{
			for ( i = 1; i < n; i++ )
			{
				BP -= 2;
				push( c, rd16( c, V30_SS, BP ) );
			}
			push( c, v16 );
		}
		BP = v16;
		SP = (uint16_t)( SP - a16 );
		c->cycles += 16;
		return 0;
	case 0xC9:  // LEAVE
		SP = BP;
		BP = pop( c );
		return 0;
	case 0xCA:  // RETF imm16
		a16 = fetch16( c );
		c->ip = pop( c );
		c->sreg[V30_CS] = pop( c );
		SP = (uint16_t)( SP + a16 );
		c->cycles += 16;
		return 0;
	case 0xCB:  // RETF
		c->ip = pop( c );
		c->sreg[V30_CS] = pop( c );
		c->cycles += 15;
		return 0;
	case 0xCC:  // INT 3
		interrupt( c, 3 );
		return 0;
	case 0xCD:  // INT imm8
		interrupt( c, fetch8( c ) );
		return 0;
	case 0xCE:  // INTO
		if ( FLAG( V30_OF ) )
		{
			interrupt( c, 4 );
		}
		return 0;
	case 0xCF:  // IRET
		c->ip = pop( c );
		c->sreg[V30_CS] = pop( c );
		c->flags = (uint16_t)( pop( c ) & 0x0FD5 );
		c->cycles += 20;
		return 0;
	case 0xD4:  // AAM (the V30 always divides by 10)
		fetch8( c );
		a16 = (uint8_t)AX;
		AX = (uint16_t)( ( ( a16 / 10 ) << 8 ) | ( a16 % 10 ) );
		set_szp( c, AX & 0xFF, 0 );
		c->cycles += 12;
		return 0;
	case 0xD5:  // AAD
		fetch8( c );
		a16 = (uint16_t)( ( ( AX >> 8 ) * 10 + ( AX & 0xFF ) ) & 0xFF );
		AX = a16;
		set_szp( c, a16, 0 );
		c->cycles += 6;
		return 0;
	case 0xD6:  // SALC (undocumented)
		set_reg8( c, 0, FLAG( V30_CF ) ? 0xFF : 0x00 );
		return 0;
	case 0xD7:  // XLAT
		set_reg8( c, 0, rd8( c, seg_of( c, V30_DS ), (uint16_t)( BX + (uint8_t)AX ) ) );
		return 0;
	case 0xE0:  // LOOPNE
	case 0xE1:  // LOOPE
	case 0xE2:  // LOOP
		s32 = (int8_t)fetch8( c );
		CX--;
		if ( CX != 0 && ( op == 0xE2 || ( op == 0xE1 ) == FLAG( V30_ZF ) ) )
		{
			c->ip = (uint16_t)( c->ip + s32 );
			c->cycles += 8;
		}
		return 0;
	case 0xE3:  // JCXZ
		s32 = (int8_t)fetch8( c );
		if ( CX == 0 )
		{
			c->ip = (uint16_t)( c->ip + s32 );
		}
		return 0;
	case 0xE4:  // IN AL, imm8
		n = fetch8( c );
		set_reg8( c, 0, c->in8 ? c->in8( (uint16_t)n ) : 0xFF );
		return 0;
	case 0xE5:  // IN AX, imm8
		n = fetch8( c );
		AX = c->in16 ? c->in16( (uint16_t)n ) : 0xFFFF;
		return 0;
	case 0xE6:  // OUT imm8, AL
		n = fetch8( c );
		if ( c->out8 )
		{
			c->out8( (uint16_t)n, (uint8_t)AX );
		}
		return 0;
	case 0xE7:  // OUT imm8, AX
		n = fetch8( c );
		if ( c->out16 )
		{
			c->out16( (uint16_t)n, AX );
		}
		return 0;
	case 0xE8:  // CALL near
		a16 = fetch16( c );
		push( c, c->ip );
		c->ip = (uint16_t)( c->ip + a16 );
		c->cycles += 12;
		return 0;
	case 0xE9:  // JMP near
		a16 = fetch16( c );
		c->ip = (uint16_t)( c->ip + a16 );
		c->cycles += 8;
		return 0;
	case 0xEA:  // JMP far
		off = fetch16( c );
		c->sreg[V30_CS] = fetch16( c );
		c->ip = off;
		c->cycles += 10;
		return 0;
	case 0xEB:  // JMP short
		s32 = (int8_t)fetch8( c );
		c->ip = (uint16_t)( c->ip + s32 );
		c->cycles += 8;
		return 0;
	case 0xEC:  // IN AL, DX
		set_reg8( c, 0, c->in8 ? c->in8( DX ) : 0xFF );
		return 0;
	case 0xED:  // IN AX, DX
		AX = c->in16 ? c->in16( DX ) : 0xFFFF;
		return 0;
	case 0xEE:  // OUT DX, AL
		if ( c->out8 )
		{
			c->out8( DX, (uint8_t)AX );
		}
		return 0;
	case 0xEF:  // OUT DX, AX
		if ( c->out16 )
		{
			c->out16( DX, AX );
		}
		return 0;
	case 0xF4:  // HLT
		c->halted = 1;
		return 0;
	case 0xF5:  // CMC
		c->flags ^= V30_CF;
		return 0;
	case 0xF6:  // group 3 r/m8
	case 0xF7:  // group 3 r/m16
	{
		uint32_t v, q, rem, d;
		int64_t num, sq;
		int64_t den;

		m = fetch8( c );
		modrm( c, m );
		w = op & 1;
		n = ( m >> 3 ) & 7;
		v = w ? get_rm16( c ) : get_rm8( c );
		switch ( n )
		{
		case 0:
		case 1:  // TEST r/m, imm
			alu( c, V30_AND, v, w ? fetch16( c ) : fetch8( c ), w );
			return 0;
		case 2:  // NOT
			if ( w )
			{
				set_rm16( c, (uint16_t)~v );
			}
			else
			{
				set_rm8( c, (uint8_t)~v );
			}
			return 0;
		case 3:  // NEG
			r32 = alu( c, V30_SUB, 0, v, w );
			set_flag( c, V30_CF, v != 0 );
			if ( w )
			{
				set_rm16( c, (uint16_t)r32 );
			}
			else
			{
				set_rm8( c, (uint8_t)r32 );
			}
			return 0;
		case 4:  // MUL
			c->cycles += 25;
			if ( w )
			{
				r32 = (uint32_t)AX * v;
				AX = (uint16_t)r32;
				DX = (uint16_t)( r32 >> 16 );
				set_flag( c, V30_CF, DX != 0 );
				set_flag( c, V30_OF, DX != 0 );
			}
			else
			{
				AX = (uint16_t)( ( AX & 0xFF ) * v );
				set_flag( c, V30_CF, ( AX >> 8 ) != 0 );
				set_flag( c, V30_OF, ( AX >> 8 ) != 0 );
			}
			set_flag( c, V30_ZF, w ? ( AX | DX ) == 0 : AX == 0 );
			return 0;
		case 5:  // IMUL
			c->cycles += 30;
			if ( w )
			{
				s32 = (int16_t)AX * (int16_t)v;
				AX = (uint16_t)s32;
				DX = (uint16_t)( (uint32_t)s32 >> 16 );
				set_flag( c, V30_CF, s32 != (int16_t)s32 );
				set_flag( c, V30_OF, s32 != (int16_t)s32 );
			}
			else
			{
				s32 = (int8_t)AX * (int8_t)v;
				AX = (uint16_t)s32;
				set_flag( c, V30_CF, s32 != (int8_t)s32 );
				set_flag( c, V30_OF, s32 != (int8_t)s32 );
			}
			return 0;
		case 6:  // DIV
			c->cycles += 40;
			if ( v == 0 )
			{
				interrupt( c, 0 );
				return 0;
			}
			if ( w )
			{
				d = ( (uint32_t)DX << 16 ) | AX;
				q = d / v;
				rem = d % v;
				if ( q > 0xFFFF )
				{
					interrupt( c, 0 );
					return 0;
				}
				AX = (uint16_t)q;
				DX = (uint16_t)rem;
			}
			else
			{
				q = AX / v;
				rem = AX % v;
				if ( q > 0xFF )
				{
					interrupt( c, 0 );
					return 0;
				}
				AX = (uint16_t)( ( rem << 8 ) | q );
			}
			return 0;
		default:  // IDIV
			c->cycles += 45;
			den = w ? (int16_t)v : (int8_t)v;
			if ( den == 0 )
			{
				interrupt( c, 0 );
				return 0;
			}
			num = w ? (int32_t)( ( (uint32_t)DX << 16 ) | AX ) : (int16_t)AX;
			sq = num / den;
			if ( ( w && ( sq > 32767 || sq < -32768 ) ) || ( !w && ( sq > 127 || sq < -128 ) ) )
			{
				interrupt( c, 0 );
				return 0;
			}
			if ( w )
			{
				AX = (uint16_t)sq;
				DX = (uint16_t)( num % den );
			}
			else
			{
				AX = (uint16_t)( ( (uint8_t)( num % den ) << 8 ) | (uint8_t)sq );
			}
			return 0;
		}
	}
	case 0xF8:  // CLC
		c->flags &= (uint16_t)~V30_CF;
		return 0;
	case 0xF9:  // STC
		c->flags |= V30_CF;
		return 0;
	case 0xFA:  // CLI
		c->flags &= (uint16_t)~V30_IF;
		return 0;
	case 0xFB:  // STI: interrupts are taken after the next instruction
		if ( !FLAG( V30_IF ) )
		{
			c->inhibit = 1;
		}
		c->flags |= V30_IF;
		return 0;
	case 0xFC:  // CLD
		c->flags &= (uint16_t)~V30_DF;
		return 0;
	case 0xFD:  // STD
		c->flags |= V30_DF;
		return 0;
	case 0xFE:  // group 4: INC / DEC r/m8
		m = fetch8( c );
		modrm( c, m );
		n = ( m >> 3 ) & 7;
		if ( n > 1 )
		{
			return -1;
		}
		set_rm8( c, (uint8_t)inc_dec( c, get_rm8( c ), n, 0 ) );
		return 0;
	case 0xFF:  // group 5
		m = fetch8( c );
		modrm( c, m );
		n = ( m >> 3 ) & 7;
		switch ( n )
		{
		case 0:
		case 1:  // INC / DEC r/m16
			set_rm16( c, (uint16_t)inc_dec( c, get_rm16( c ), n, 1 ) );
			return 0;
		case 2:  // CALL near r/m16
			v16 = get_rm16( c );
			push( c, c->ip );
			c->ip = v16;
			c->cycles += 12;
			return 0;
		case 3:  // CALL far m16:16
			off = rd16( c, c->ea_seg, c->ea_off );
			seg = rd16( c, c->ea_seg, (uint16_t)( c->ea_off + 2 ) );
			push( c, c->sreg[V30_CS] );
			push( c, c->ip );
			c->ip = off;
			c->sreg[V30_CS] = seg;
			c->cycles += 20;
			return 0;
		case 4:  // JMP near r/m16
			c->ip = get_rm16( c );
			c->cycles += 8;
			return 0;
		case 5:  // JMP far m16:16
			off = rd16( c, c->ea_seg, c->ea_off );
			c->sreg[V30_CS] = rd16( c, c->ea_seg, (uint16_t)( c->ea_off + 2 ) );
			c->ip = off;
			c->cycles += 12;
			return 0;
		case 6:
		case 7:  // PUSH r/m16
			push( c, get_rm16( c ) );
			return 0;
		}
		return 0;
	default:
		break;
	}
	(void)lin;
	return -1;
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Reset: CS:IP = FFFF:0000, flags clear, interrupts off.
  *
  * @param  c The CPU (mem and callbacks are kept).
  *
  * @retval None.
  */
void v30_reset( v30_cpu *c )
{
	if ( !v30_tables_ready )
	{
		v30_tables();
	}
	memset( c->r, 0, sizeof c->r );
	memset( c->sreg, 0, sizeof c->sreg );
	c->sreg[V30_CS] = 0xFFFF;
	c->ip = 0;
	c->flags = 0;
	c->halted = 0;
	c->nmi = 0;
	c->inhibit = 0;
}

/**
  * @brief  Execute instructions for about the given number of clock cycles.
  *
  * @param  c The CPU.
  * @param  cycles Clock cycles to run.
  *
  * @retval Cycles actually run.
  */
int v30_run( v30_cpu *c, int cycles )
{
	uint64_t start = c->cycles, end = c->cycles + (uint64_t)cycles;

	while ( c->cycles < end )
	{
		if ( c->nmi )
		{
			c->nmi = 0;
			interrupt( c, 2 );
		}
		else if ( c->inhibit )
		{
			c->inhibit = 0;
		}
		else if ( FLAG( V30_IF ) && c->irq_pending && c->irq_pending() )
		{
			interrupt( c, c->irq_ack() );
		}
		if ( c->halted )
		{
			c->cycles = end;
			break;
		}
		if ( step( c ) != 0 )
		{
			dw_log( "%10.6f v30: undefined opcode %02X at %04X:%04X\n", (double)dw_cycles / DW_CPU_HZ,
					c->mem[V30_LIN( c->prev_cs, c->prev_ip )], c->prev_cs, c->prev_ip );
		}
	}
	return (int)( c->cycles - start );
}

/**
  * @brief  Disassemble-free one-line state dump for logs.
  *
  * @param  c The CPU.
  * @param  out Receives the text.
  * @param  n Room in out.
  *
  * @retval None.
  */
void v30_state( const v30_cpu *c, char *out, int n )
{
	snprintf( out, (size_t)n,
			  "CS:IP=%04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X "
			  "DS=%04X ES=%04X SS=%04X F=%04X%s",
			  c->sreg[V30_CS], c->ip, c->r[V30_AX], c->r[V30_BX], c->r[V30_CX], c->r[V30_DX],
			  c->r[V30_SI], c->r[V30_DI], c->r[V30_BP], c->r[V30_SP], c->sreg[V30_DS],
			  c->sreg[V30_ES], c->sreg[V30_SS], c->flags, c->halted ? " HLT" : "" );
}
