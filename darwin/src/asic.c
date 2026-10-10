//  Description:
//
//		asic.c -- E-mu audio ASIC, shared by the 68306 and the V50.
//
//		What is modelled so far: the shared register file, the two IPC FIFOs
//		with their status and interrupt bits on each side, the 68306's port
//		into V50 memory (program upload, ROM 01BB3E), the V50 hold / run
//		control, the V50 interrupt vectoring with end-of-interrupt, and the
//		data wheel. Audio streaming and the meters come later.
//
//		The FIFO depth is not known; the firmware on both sides waits for
//		"room" before each word, so any depth works. 32 words are used.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "asic.h"
#include "audio.h"
#include "darwin.h"
#include "sim68306.h"
#include "v50.h"
#include "wd33c93.h"

#include <math.h>
#include <string.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define ASIC_WORDS    0x100   // 0x000-0x1FF
#define ASIC_FIFO     32

#define ASIC_E0_WRAP  0x0004  // the sample counter wrapped (24 bits)
#define ASIC_E0_TICK  0x0008

#define ASIC_RATE     44100   // sample clock, Hz (the 48 kHz select is not found yet)
#define ASIC_SPEED_1X 0x40    // C2 value for normal speed
#define ASIC_COUNT_MASK 0xFFFFFFu

// the memory the ASIC addresses (streaming, SCSI DMA, the E2 port). The low megabyte is the V50's own
// RAM; above it is track buffer RAM the V50 cannot see. The firmware's 64 KB block pool reaches
// 2EFFFF (2 MB of buffer RAM), so a 4 MB space; with only 2 MB, the blocks from 200000 up wrapped
// onto the V50's code and vectors, and an 8-track playback hung the disk engine
#define ASIC_MEM_SIZE 0x400000u
#define ASIC_SILENCE  0x040000u  // the V50's silence block (64 KB), see asic_stream
#define ASIC_ST_CLOCK 0x4000  // F6 (V50) / FE (68306) bit 14: the sample clock is locked (00B108)

// the analog level switches (Routing screen, In Pad / Out Pad) are 68306 port A pins, not ASIC
// registers: bit 6 set = inputs at -10 dBV, bit 5 set = outputs at +4 dBu (ROM 04D63E writes them).
// The two levels are 11.8 dB apart (1.228 V against 0.316 V); a host sample counts as a -10 input
// and a +4 output, so the other setting is that much quieter
#define ASIC_PA_IN_10 0x40
#define ASIC_PA_OUT_4 0x20
#define ASIC_PAD      0.2572f  // -11.8 dB

// per-track streaming registers (32-bit, V50 order): the current block and the next one
#define ASIC_CUR_ADDR( t )  ( 0x100 + 4 * ( t ) )
#define ASIC_CUR_CNT( t )   ( 0x120 + 4 * ( t ) )  // words left - 1
#define ASIC_NEXT_ADDR( t ) ( 0x140 + 4 * ( t ) )
#define ASIC_NEXT_CNT( t )  ( 0x160 + 4 * ( t ) )

#define ASIC_ST_ROOM  0x0001  // F4 bit 0: the outgoing FIFO has room
#define ASIC_ST_DATA  0x0002  // F4 bit 1: a word is waiting
#define ASIC_ST_TXEN  0x0004  // F4 bit 2: interrupt when there is room

#define ASIC_INT_EVENT 9      // V50 vectors
#define ASIC_INT_RX    10
#define ASIC_INT_TX    11
#define ASIC_INT_SYNC  12     // the counter reached the compare register
#define ASIC_INT_SCSI1 13     // audio bus controller
#define ASIC_INT_SCSI0 14     // host bus controller

// SCSI DMA channels: chip 0 (host bus) uses 3188 / 318C and control 30FA, chip 1 (audio bus) uses
// 3180 / 3184 and control 30F8 (V50 program 003D75); addresses and counts are two words, low first
#define ASIC_DMA_ADDR( chip ) ( ( chip ) ? 0x180 : 0x188 )
#define ASIC_DMA_CNT( chip )  ( ( chip ) ? 0x184 : 0x18C )
#define ASIC_DMA_CTL( chip )  ( ( chip ) ? 0xF8 : 0xFA )

//------------------------------------------------------------------------------
//---------------------- Local types and structures ----------------------------
//------------------------------------------------------------------------------

typedef struct
{
	uint16_t buf[ASIC_FIFO];
	int      r, n;
	uint16_t last;  // read when empty
} asic_fifo;

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static uint16_t  asic_reg[ASIC_WORDS];      // shared register file
static uint16_t  asic_ctl68[0x20];          // 68306's own C0-FF block
static uint16_t  asic_ctl50[0x20];          // V50's own C0-FF block
static asic_fifo asic_to50, asic_to68;
static uint16_t  asic_en68, asic_en50;      // F4 enables written by each side
static uint16_t  asic_e0;                   // 68306 interrupt causes
static uint16_t  asic_e4;                   // V50 event causes
static int       asic_in_service;           // V50 vector being serviced, 0 = none
static uint8_t  *asic_v50_mem;
static uint32_t  asic_max_addr;  // the highest address used (the end-of-run log)
static uint8_t   asic_hi_mem[ASIC_MEM_SIZE - 0x100000];  // above the first megabyte: buffer RAM the V50 cannot see
static uint32_t  asic_port_addr;            // 198: address of the E2 port
static uint64_t  asic_phase;                // sample clock phase, in 68306 cycles x rate
static unsigned  asic_speed_acc;            // counter steps owed, in 1/64 sample
static int       asic_sync;                 // INT 12 latched
static uint32_t  asic_dma_left[2];          // bytes a SCSI DMA channel may still move
static int16_t   asic_in[AU_INS];           // the converters' latest input samples
static int16_t   asic_trk[8];               // what each track carries this sample
static int16_t   asic_mix[2];               // the mixer's last output, L / R (bouncing)
static uint16_t  asic_peak[8];              // largest |sample| per track since the meter task read it
static unsigned  asic_logged[2][ASIC_WORDS];
static unsigned  asic_ipc_logged;            // IPC words logged so far

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void asic_fifo_put( asic_fifo *f, uint16_t v );
static uint16_t asic_fifo_get( asic_fifo *f );
static int asic_v50_pending( void );
static void asic_log( int side, int write, unsigned off, uint16_t v );
static unsigned asic_off68( unsigned off );
static int asic_shared_ctl( unsigned off );
static uint32_t asic_long( unsigned off );
static void asic_set_long( unsigned off, uint32_t v );
static void asic_step( int dir );
static uint8_t *asic_mem( uint32_t addr );
static int16_t asic_track_input( int t, float pad );
static uint16_t asic_meter_level( uint16_t peak );
static void asic_stream( const int16_t rec[8], int16_t play[8] );
static void asic_sample( void );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Append a word to a FIFO (dropped, with a log line, when full).
  *
  * @param  f The FIFO.
  * @param  v The word.
  *
  * @retval None.
  */
static void asic_fifo_put( asic_fifo *f, uint16_t v )
{
	if ( f->n == ASIC_FIFO )
	{
		dw_log( "asic: IPC FIFO overflow, word %04X lost\n", v );
		return;
	}
	f->buf[( f->r + f->n ) % ASIC_FIFO] = v;
	f->n++;
}

/**
  * @brief  Take the oldest word from a FIFO.
  *
  * @param  f The FIFO.
  *
  * @retval The word; the last one again when empty.
  */
static uint16_t asic_fifo_get( asic_fifo *f )
{
	if ( f->n == 0 )
	{
		return f->last;
	}
	f->last = f->buf[f->r];
	f->r = ( f->r + 1 ) % ASIC_FIFO;
	f->n--;
	return f->last;
}

/**
  * @brief  The V50 interrupt sources that want service, as a vector bit mask.
  *
  * @retval Bit n set = INT n requested.
  */
static int asic_v50_pending( void )
{
	int m = 0;

	if ( asic_e4 )
	{
		m |= 1 << ASIC_INT_EVENT;
	}
	if ( asic_sync )
	{
		m |= 1 << ASIC_INT_SYNC;
	}
	if ( asic_to50.n > 0 )
	{
		m |= 1 << ASIC_INT_RX;
	}
	if ( ( asic_en50 & ASIC_ST_TXEN ) && asic_to68.n < ASIC_FIFO )
	{
		m |= 1 << ASIC_INT_TX;
	}
	if ( wd_irq( 1 ) )
	{
		m |= 1 << ASIC_INT_SCSI1;
	}
	if ( wd_irq( 0 ) )
	{
		m |= 1 << ASIC_INT_SCSI0;
	}
	return m;
}

/**
  * @brief  Log the first few accesses to each register (-io).
  *
  * @param  side 0 = 68306, 1 = V50.
  * @param  write 1 for a write.
  * @param  off Register offset.
  * @param  v The value.
  *
  * @retval None.
  */
static void asic_log( int side, int write, unsigned off, uint16_t v )
{
	if ( dw_trace_asic && dw_cycles >= dw_trace_asic )
	{
		if ( !write && ( off == 0x194 || off == 0x196 || off == 0xF4 || off == 0xF6 || off == 0xFE ) )
		{
			return;  // polled all the time: they would drown the rest
		}
		if ( side )
		{
			dw_log( "%10.6f V50 %c %03X %04X  %04X:%04X\n", (double)dw_cycles / DW_CPU_HZ, write ? 'W' : 'R', off, v,
					v50_where() >> 16, v50_where() & 0xFFFF );
		}
		else
		{
			dw_log( "%10.6f 68k %c %03X %04X  %06X\n", (double)dw_cycles / DW_CPU_HZ, write ? 'W' : 'R', off, v, dw_pc() );
		}
		return;
	}
	if ( !dw_log_io || asic_logged[side][( off >> 1 ) & ( ASIC_WORDS - 1 )]++ >= 4 )
	{
		return;
	}
	dw_log( "asic %s: %s %03X %s %04X\n", side ? "V50" : "68k", write ? "write" : "read ", off,
			write ? "<-" : "->", v );
}

/**
  * @brief  Where a 68306 offset lands in the register file.
  *
  * The file is kept in the V50's order; the 68306 sees the 32-bit
  * registers with their words swapped (its high word first).
  *
  * @param  off 68306 offset.
  *
  * @retval Register file offset.
  */
static unsigned asic_off68( unsigned off )
{
	return off >= 0x100 && off < 0x1C0 ? off ^ 2 : off;
}

/**
  * @brief  Is a control-block offset one register for both sides?
  *
  * @param  off Offset C0-FF.
  *
  * @retval 1 shared (speed C2, track source CA, counter run EA), 0 each side's own.
  */
static int asic_shared_ctl( unsigned off )
{
	return off == 0xC2 || off == 0xCA || off == 0xEA;
}

/**
  * @brief  A 32-bit register (V50 order: low word first).
  *
  * @param  off Its offset.
  *
  * @retval The value.
  */
static uint32_t asic_long( unsigned off )
{
	return (uint32_t)asic_reg[off >> 1] | (uint32_t)asic_reg[( off >> 1 ) + 1] << 16;
}

/**
  * @brief  Set a 32-bit register.
  *
  * @param  off Its offset.
  * @param  v The value.
  *
  * @retval None.
  */
static void asic_set_long( unsigned off, uint32_t v )
{
	asic_reg[off >> 1] = (uint16_t)v;
	asic_reg[( off >> 1 ) + 1] = (uint16_t)( v >> 16 );
}

/**
  * @brief  A byte of the ASIC's memory: the V50's RAM below 100000, buffer RAM above.
  *
  * @param  addr ASIC address (wraps at ASIC_MEM_SIZE).
  *
  * @retval Pointer to the byte.
  */
static uint8_t *asic_mem( uint32_t addr )
{
	addr &= ASIC_MEM_SIZE - 1;
	if ( addr > asic_max_addr )
	{
		asic_max_addr = addr;
	}
	return addr < 0x100000 ? asic_v50_mem + addr : asic_hi_mem + ( addr - 0x100000 );
}

/**
  * @brief  Move the sample counter one sample.
  *
  * A wrap of the 24 bits interrupts the 68306 (it keeps the top byte in
  * software); reaching the compare register interrupts the V50.
  *
  * @param  dir +1 forward, -1 backward.
  *
  * @retval None.
  */
static void asic_step( int dir )
{
	uint32_t c = asic_long( 0x194 ) & ASIC_COUNT_MASK;

	c = ( c + (uint32_t)dir ) & ASIC_COUNT_MASK;
	if ( c == ( dir > 0 ? 0 : ASIC_COUNT_MASK ) )
	{
		asic_e0 |= ASIC_E0_WRAP;
	}
	asic_set_long( 0x194, c );
	if ( c == ( asic_long( 0x190 ) & ASIC_COUNT_MASK ) )
	{
		asic_sync = 1;
	}
}

/**
  * @brief  The input a track records from (BC / BE: a nibble per track).
  *
  * 0-7 are analog inputs 1-8 (5-8 on the input expander, which the
  * firmware finds fitted: its routing screen shows ANA 1-8), A / B the
  * S/PDIF input L / R (the clock watch at ROM 00B108 unroutes exactly
  * these when the clock is lost); 8 / 9 are a guess for the mix L / R
  * (bouncing); F is none.
  *
  * @param  t Track 0-7.
  * @param  pad Gain of the analog inputs (the In Pad switch).
  *
  * @retval The sample.
  */
static int16_t asic_track_input( int t, float pad )
{
	unsigned src = ( asic_reg[( t < 4 ? 0xBC : 0xBE ) >> 1] >> ( 4 * ( t & 3 ) ) ) & 0xF;

	if ( src < 8 )
	{
		return (int16_t)( asic_in[src] * pad );
	}
	if ( src == 0xA || src == 0xB )
	{
		return asic_in[8 + src - 0xA];
	}
	if ( src == 8 || src == 9 )
	{
		return asic_mix[src - 8];
	}
	return 0;
}

/**
  * @brief  A track's meter register: its peak on the ASIC's log scale.
  *
  * 3F is full scale and each step 1.5 dB down: the meter task's table
  * (ROM 00A7E0) lights the -3 dB segment at 3D, -6 at 3B ... -44 at 24.
  *
  * @param  peak Largest |sample|, 0-32768.
  *
  * @retval 0-3F.
  */
static uint16_t asic_meter_level( uint16_t peak )
{
	double steps;

	if ( peak == 0 )
	{
		return 0;
	}
	steps = 20.0 * log10( peak / 32767.0 ) / 1.5;
	return (uint16_t)( steps >= 0 ? 0x3F : steps < -63 ? 0 : 0x3F + (int)floor( steps ) );
}

/**
  * @brief  Move one word through every streaming track's buffer.
  *
  * Per track t: 30C0 bit t enables it, 30B8 bit t runs it (set at the
  * punch-in point when recording, at once when playing), 30BA bit t is
  * the direction (1 play from memory, 0 record into it). When a block's
  * count runs out the next block becomes current and event bit t (30E4,
  * INT 9) asks the V50 for another.
  *
  * @param  rec The samples to record, per track.
  * @param  play Receives the samples played, per track (left alone otherwise).
  *
  * @retval None.
  */
static void asic_stream( const int16_t rec[8], int16_t play[8] )
{
	uint16_t run = asic_ctl50[0] & asic_reg[0xB8 >> 1], dir = asic_reg[0xBA >> 1];
	uint32_t addr, cnt;
	uint8_t *p;
	int t;

	for ( t = 0; t < 8; t++ )
	{
		if ( !( run & ( 1u << t ) ) )
		{
			continue;
		}
		addr = asic_long( ASIC_CUR_ADDR( t ) ) & ( ASIC_MEM_SIZE - 2 );
		cnt = asic_long( ASIC_CUR_CNT( t ) );
		p = asic_mem( addr );
		if ( dir & ( 1u << t ) )
		{
			// the V50 queues 040000-04FFFF as silence wherever a track has no audio, but nothing ever
			// clears it: its boot stack and RAM probe live there, and on the hardware it would hold
			// power-up noise too. The hardware evidently plays nothing from it; neither does this
			play[t] = addr - ASIC_SILENCE < 0x10000 ? 0 : (int16_t)( p[0] | p[1] << 8 );
		}
		else
		{
			p[0] = (uint8_t)rec[t];
			p[1] = (uint8_t)( (uint16_t)rec[t] >> 8 );
		}
		if ( cnt == 0 )
		{
			asic_set_long( ASIC_CUR_ADDR( t ), asic_long( ASIC_NEXT_ADDR( t ) ) );
			asic_set_long( ASIC_CUR_CNT( t ), asic_long( ASIC_NEXT_CNT( t ) ) );
			asic_e4 |= (uint16_t)( 1u << t );
		}
		else
		{
			asic_set_long( ASIC_CUR_ADDR( t ), ( addr + 2 ) & ( ASIC_MEM_SIZE - 1 ) );
			asic_set_long( ASIC_CUR_CNT( t ), cnt - 1 );
		}
	}
}

/**
  * @brief  One period of the sample clock.
  *
  * The counter moves by speed / 40h samples (several while winding, a
  * fraction while shuttling); the tracks stream once per counter step
  * at normal speed. Then the converters: what each track carries (CA bit
  * t set: its input, as routed by BC / BE; clear: what it plays; the
  * 68306 sets this per transport state and arming, ROM 03F8CA), the
  * meters, the 8 x 2 mixer and the frame out to the host (audio.c).
  *
  * The mixer, as the 68306 sets it per track t: 050 + 2t and 070 + 2t a
  * pan pair (B5 each = centre, -3 dB; which is left is a guess), 090 + 2t
  * the track's switch (FF on, 0 muted: a track that is neither monitored
  * nor playing). The analog inputs and track outputs pass the In / Out
  * Pad switches (port A); S/PDIF and the headphones do not.
  *
  * @retval None.
  */
static void asic_sample( void )
{
	int16_t speed = (int16_t)asic_reg[0xC2 >> 1], out[AU_OUTS], rec[8], play[8];
	uint16_t live = asic_reg[0xCA >> 1], level;
	int32_t mix[2], s;
	float phones = au_phones_gain();
	uint8_t pa = sim_port_out( 0 );
	float in_pad = pa & ASIC_PA_IN_10 ? 1.0f : ASIC_PAD, out_pad = pa & ASIC_PA_OUT_4 ? 1.0f : ASIC_PAD;
	int t, dir;

	for ( t = 0; t < 8; t++ )
	{
		rec[t] = asic_track_input( t, in_pad );
		play[t] = 0;
	}
	if ( !( asic_reg[0xEA >> 1] & 1 ) && speed != 0 )  // EA bit 0 holds the counter
	{
		dir = speed > 0 ? 1 : -1;
		asic_speed_acc += (unsigned)( speed > 0 ? speed : -speed );
		while ( asic_speed_acc >= ASIC_SPEED_1X )
		{
			asic_speed_acc -= ASIC_SPEED_1X;
			asic_step( dir );
			if ( speed == ASIC_SPEED_1X )
			{
				asic_stream( rec, play );
			}
		}
	}
	mix[0] = mix[1] = 0;
	for ( t = 0; t < 8; t++ )
	{
		asic_trk[t] = live & ( 1u << t ) ? rec[t] : play[t];
		out[t] = (int16_t)( asic_trk[t] * out_pad );  // the analog track outputs, after the Out Pad switch
		s = asic_trk[t] * (int32_t)( asic_reg[( 0x90 >> 1 ) + t] & 0xFF ) / 255;
		mix[0] += s * (int32_t)( asic_reg[( 0x50 >> 1 ) + t] & 0xFF ) / 256;
		mix[1] += s * (int32_t)( asic_reg[( 0x70 >> 1 ) + t] & 0xFF ) / 256;
		level = (uint16_t)( asic_trk[t] < 0 ? -asic_trk[t] : asic_trk[t] );
		if ( level > asic_peak[t] )
		{
			asic_peak[t] = level;  // held until the 68306 reads and clears the meter register
		}
	}
	for ( t = 0; t < 2; t++ )
	{
		asic_mix[t] = (int16_t)( mix[t] > 32767 ? 32767 : mix[t] < -32768 ? -32768 : mix[t] );
		out[8 + t] = (int16_t)( asic_mix[t] * phones );  // headphones: the mix after the volume knob
		out[10 + t] = asic_mix[t];                       // S/PDIF out: the mix as it is
	}
	au_frame( out, asic_in );
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Power-on reset.
  *
  * @param  v50_mem The V50's 1 MB memory (for the E2 / 198 port).
  *
  * @retval None.
  */
void asic_reset( uint8_t *v50_mem )
{
	memset( asic_reg, 0, sizeof asic_reg );
	memset( asic_ctl68, 0, sizeof asic_ctl68 );
	memset( asic_ctl50, 0, sizeof asic_ctl50 );
	memset( &asic_to50, 0, sizeof asic_to50 );
	memset( &asic_to68, 0, sizeof asic_to68 );
	memset( asic_logged, 0, sizeof asic_logged );
	asic_phase = 0;
	asic_speed_acc = 0;
	asic_sync = 0;
	asic_dma_left[0] = asic_dma_left[1] = 0;
	memset( asic_in, 0, sizeof asic_in );
	memset( asic_trk, 0, sizeof asic_trk );
	memset( asic_mix, 0, sizeof asic_mix );
	memset( asic_peak, 0, sizeof asic_peak );
	asic_en68 = asic_en50 = 0;
	asic_e0 = asic_e4 = 0;
	asic_in_service = 0;
	asic_v50_mem = v50_mem;
	asic_port_addr = 0;
	asic_ctl68[0xEC >> 1 & 0x1F] = 6;  // the V50 is held until the 68306 releases it
}

/**
  * @brief  68306 side: read a 16-bit register.
  *
  * @param  a Address, C00000-CFFFFF (even).
  *
  * @retval The word.
  */
uint16_t asic_read16( uint32_t a )
{
	unsigned off = a & 0x1FE;
	uint16_t v;

	switch ( off )
	{
	case 0xC6:
		v = asic_fifo_get( &asic_to68 );
		dw_irq_update();
		break;
	case 0xE0:
		v = asic_e0;
		break;
	case 0xFE:
		v = (uint16_t)( ( asic_ctl68[( 0xFE - 0xC0 ) >> 1] & ~ASIC_ST_CLOCK ) | ASIC_ST_CLOCK );  // the sample clock is always locked
		break;
	case 0xA0: case 0xA2: case 0xA4: case 0xA6: case 0xA8: case 0xAA: case 0xAC: case 0xAE:
		v = asic_meter_level( asic_peak[( off - 0xA0 ) >> 1] );
		break;
	case 0xE2:  // V50 memory, little-endian word, auto-increment
		v = (uint16_t)( *asic_mem( asic_port_addr ) | *asic_mem( asic_port_addr + 1 ) << 8 );
		asic_port_addr = ( asic_port_addr + 2 ) & ( ASIC_MEM_SIZE - 1 );
		break;
	case 0xF4:
		v = (uint16_t)( ( asic_en68 & ~3u ) | ( asic_to50.n < ASIC_FIFO ? ASIC_ST_ROOM : 0 ) |
						( asic_to68.n > 0 ? ASIC_ST_DATA : 0 ) );
		break;
	default:
		if ( off >= 0xC0 && off < 0x100 && !asic_shared_ctl( off ) )
		{
			v = asic_ctl68[( off - 0xC0 ) >> 1];
		}
		else
		{
			v = asic_reg[asic_off68( off ) >> 1];
		}
		break;
	}
	asic_log( 0, 0, off, v );
	return v;
}

/**
  * @brief  68306 side: write a 16-bit register.
  *
  * @param  a Address, C00000-CFFFFF (even).
  * @param  v The word.
  *
  * @retval None.
  */
void asic_write16( uint32_t a, uint16_t v )
{
	unsigned off = a & 0x1FE;

	asic_log( 0, 1, off, v );
	switch ( off )
	{
	case 0xC8:
		if ( asic_ipc_logged++ < 2000 )
		{
			dw_log( "ipc 68k->V50 %04X (PC %06X)\n", v, dw_pc() );
		}
		asic_fifo_put( &asic_to50, v );
		break;
	case 0xA0: case 0xA2: case 0xA4: case 0xA6: case 0xA8: case 0xAA: case 0xAC: case 0xAE:
		asic_peak[( off - 0xA0 ) >> 1] = 0;  // the meter task clears a peak after reading it
		break;
	case 0xE0:  // acknowledge: a set bit clears that cause
		asic_e0 &= (uint16_t)~( v & ( ASIC_E0_WRAP | ASIC_E0_TICK ) );
		break;
	case 0xE2:
		*asic_mem( asic_port_addr ) = (uint8_t)v;
		*asic_mem( asic_port_addr + 1 ) = (uint8_t)( v >> 8 );
		asic_port_addr = ( asic_port_addr + 2 ) & ( ASIC_MEM_SIZE - 1 );
		break;
	case 0xF4:
		asic_en68 = v;
		break;
	case 0x198:
	case 0x19A:
		asic_reg[asic_off68( off ) >> 1] = v;
		asic_port_addr = asic_long( 0x198 ) & ( ASIC_MEM_SIZE - 1 );
		break;
	default:
		if ( off >= 0xC0 && off < 0x100 && !asic_shared_ctl( off ) )
		{
			asic_ctl68[( off - 0xC0 ) >> 1] = v;
		}
		else
		{
			asic_reg[asic_off68( off ) >> 1] = v;
		}
		break;
	}
	dw_irq_update();
}

/**
  * @brief  V50 side: read a 16-bit register.
  *
  * @param  port I/O port, 3000-31FF.
  *
  * @retval The word.
  */
uint16_t asic_v50_in( uint16_t port )
{
	unsigned off = port & 0x1FE;
	uint16_t v;

	switch ( off )
	{
	case 0xF6:
		v = (uint16_t)( ( asic_ctl50[( 0xF6 - 0xC0 ) >> 1] & ~ASIC_ST_CLOCK ) | ASIC_ST_CLOCK );  // the sample clock is always locked
		break;
	case 0xC8:
		v = asic_fifo_get( &asic_to50 );
		dw_irq_update();
		break;
	case 0xE4:
		v = asic_e4;
		break;
	case 0xF4:
		v = (uint16_t)( ( asic_en50 & ~3u ) | ( asic_to68.n < ASIC_FIFO ? ASIC_ST_ROOM : 0 ) |
						( asic_to50.n > 0 ? ASIC_ST_DATA : 0 ) );
		break;
	default:
		v = off >= 0xC0 && off < 0x100 && !asic_shared_ctl( off ) ? asic_ctl50[( off - 0xC0 ) >> 1] : asic_reg[off >> 1];
		break;
	}
	asic_log( 1, 0, off, v );
	return v;
}

/**
  * @brief  V50 side: write a 16-bit register.
  *
  * @param  port I/O port, 3000-31FF.
  * @param  v The word.
  *
  * @retval None.
  */
void asic_v50_out( uint16_t port, uint16_t v )
{
	unsigned off = port & 0x1FE;

	asic_log( 1, 1, off, v );
	switch ( off )
	{
	case 0xC6:
		if ( asic_ipc_logged++ < 2000 )
		{
			dw_log( "ipc V50->68k %04X\n", v );
		}
		asic_fifo_put( &asic_to68, v );
		break;
	case 0xE4:  // writing the causes back acknowledges them
		asic_e4 &= (uint16_t)~v;
		break;
	case 0xF0:  // end of interrupt
		asic_in_service = 0;
		break;
	case 0xF4:
		asic_en50 = v;
		break;
	case 0x184:  // SCSI DMA byte counts: 16 bits, 0 = 64 KB (the high words are written 0)
	case 0x18C:
		asic_reg[off >> 1] = v;
		asic_dma_left[off == 0x184] = v ? v : 0x10000;
		break;
	default:
		if ( off >= 0xC0 && off < 0x100 && !asic_shared_ctl( off ) )
		{
			asic_ctl50[( off - 0xC0 ) >> 1] = v;
		}
		else
		{
			asic_reg[off >> 1] = v;
		}
		break;
	}
	dw_irq_update();
}

/**
  * @brief  Is the V50 interrupt line asserted?
  *
  * @retval 1 if asserted.
  */
int asic_v50_irq( void )
{
	return !asic_in_service && asic_v50_pending() != 0;
}

/**
  * @brief  One-line V50 interrupt state for logs.
  *
  * @param  out Receives the text.
  * @param  n Room in out.
  *
  * @retval None.
  */
void asic_v50_state( char *out, int n )
{
	int k, t;

	k = snprintf( out, (size_t)n, "highest address %06X, in service %d, pending %04X, E4 %04X, enables %04X, "
				  "FIFO to V50 %d, to 68k %d; tracks C0 %04X B8 %04X BA %04X CA %04X EA %04X",
				  (unsigned)asic_max_addr, asic_in_service, (unsigned)asic_v50_pending(), asic_e4, asic_en50,
				  asic_to50.n, asic_to68.n, asic_ctl50[0], asic_reg[0xB8 >> 1], asic_reg[0xBA >> 1],
				  asic_reg[0xCA >> 1], asic_reg[0xEA >> 1] );
	for ( t = 0; t < 8 && k > 0 && k < n; t++ )
	{
		k += snprintf( out + k, (size_t)( n - k ), "; %d: %06X+%04X", t, (unsigned)asic_long( ASIC_CUR_ADDR( t ) ),
					   (unsigned)asic_long( ASIC_CUR_CNT( t ) ) );
	}
}

/**
  * @brief  V50 interrupt acknowledge.
  *
  * @retval The vector number.
  */
int asic_v50_ack( void )
{
	int m = asic_v50_pending(), n;

	for ( n = 8; n < 16; n++ )  // lowest vector first
	{
		if ( m & ( 1 << n ) )
		{
			asic_in_service = n;
			if ( n == ASIC_INT_SYNC )  // an edge: taken once
			{
				asic_sync = 0;
			}
			return n;
		}
	}
	return 15;
}

/**
  * @brief  May the V50 run (not held for a program upload)?
  *
  * @retval 1 if it may run.
  */
int asic_v50_running( void )
{
	return asic_ctl68[( 0xEC - 0xC0 ) >> 1] != 6;
}

/**
  * @brief  Has the V50 started the DMA channel of a SCSI controller?
  *
  * @param  chip 0 host bus, 1 audio bus.
  *
  * @retval 1 started.
  */
int asic_dma_ready( int chip )
{
	return ( asic_ctl50[( ASIC_DMA_CTL( chip ) - 0xC0 ) >> 1] & 1 ) != 0;
}

/**
  * @brief  Move a SCSI data phase through a DMA channel.
  *
  * The channel's address and count registers advance as the hardware's
  * would. Addresses are V50 byte addresses.
  *
  * @param  chip 0 host bus, 1 audio bus.
  * @param  buf The SCSI side of the data.
  * @param  n Bytes the SCSI side offers or wants.
  * @param  to_mem 1 for data in (SCSI to memory), 0 for data out.
  *
  * @retval Bytes moved (at most the channel's count).
  */
uint32_t asic_dma_move( int chip, uint8_t *buf, uint32_t n, int to_mem )
{
	unsigned a = ASIC_DMA_ADDR( chip ) >> 1, c = ASIC_DMA_CNT( chip ) >> 1;
	uint32_t addr = ( (uint32_t)asic_reg[a + 1] << 16 ) | asic_reg[a];
	uint32_t cnt = asic_dma_left[chip & 1], i;

	if ( n > cnt )
	{
		n = cnt;
	}
	asic_dma_left[chip & 1] = cnt - n;
	if ( dw_log_io )
	{
		dw_log( "asic: SCSI%d DMA %s %u bytes at %06X (count %u)\n", chip, to_mem ? "in" : "out", n,
				(unsigned)addr, (unsigned)cnt );
	}
	for ( i = 0; i < n; i++ )
	{
		if ( to_mem )
		{
			*asic_mem( addr + i ) = buf[i];
		}
		else
		{
			buf[i] = *asic_mem( addr + i );
		}
	}
	addr += n;
	cnt -= n;
	asic_reg[a] = (uint16_t)addr;
	asic_reg[a + 1] = (uint16_t)( addr >> 16 );
	asic_reg[c] = (uint16_t)cnt;
	asic_reg[c + 1] = (uint16_t)( cnt >> 16 );
	return n;
}

/**
  * @brief  Advance the ASIC's timers.
  *
  * @param  cycles 68306 cycles elapsed.
  *
  * @retval None.
  */
void asic_tick( int cycles )
{
	// the sample clock always runs; one period per DW_CPU_HZ / rate cycles, kept exact in integer units
	asic_phase += (uint64_t)cycles * ASIC_RATE;
	while ( asic_phase >= DW_CPU_HZ )
	{
		asic_phase -= DW_CPU_HZ;
		asic_sample();
	}
	dw_irq_update();
}

/**
  * @brief  Is the 68306 interrupt (level 3) asserted?
  *
  * @retval 1 if asserted, 0 if not.
  */
int asic_irq( void )
{
	return asic_e0 != 0 || asic_to68.n > 0 || ( ( asic_en68 & ASIC_ST_TXEN ) && asic_to50.n < ASIC_FIFO );
}

/**
  * @brief  The transport's sample counter.
  *
  * @retval 24-bit sample position.
  */
uint32_t asic_counter( void )
{
	return asic_long( 0x194 ) & ASIC_COUNT_MASK;
}
