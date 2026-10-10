//  Description:
//
//		v50.c -- the Darwin's disk engine: an NEC V50 with 1 MB of memory.
//
//		All of the engine's memory is treated as RAM: the program lives in
//		the upload area F0000-FFFFF and copies itself to 00000, data is at
//		10000 and the stack at 40000-44000. How much RAM the real board has
//		is not known; a full megabyte is harmless.
//
//		The on-chip interrupt controller is not used: the audio ASIC supplies
//		the interrupt vectors (asic.c).
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "v50.h"
#include "asic.h"
#include "darwin.h"
#include "v30.h"
#include "wd33c93.h"

#include <stdio.h>
#include <string.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define V50_LOG_PORTS 0x10000

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

uint8_t        v50_mem[V50_MEM_SIZE];
static v30_cpu v50_cpu;
static int     v50_was_running;
static double  v50_frac;                     // V50 cycles carried between slices
static uint8_t v50_logged[V50_LOG_PORTS];    // ports already reported as unknown

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void v50_unknown( uint16_t port, int write, uint16_t v, int size );
static uint16_t v50_in16( uint16_t port );
static void v50_out16( uint16_t port, uint16_t v );
static uint8_t v50_in8( uint16_t port );
static void v50_out8( uint16_t port, uint8_t v );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Report the first access to a port nothing answers.
  *
  * @param  port The port.
  * @param  write 1 for OUT.
  * @param  v Value written.
  * @param  size 1 or 2 bytes.
  *
  * @retval None.
  */
static void v50_unknown( uint16_t port, int write, uint16_t v, int size )
{
	if ( v50_logged[port] && !dw_log_io )
	{
		return;
	}
	v50_logged[port] = 1;
	if ( write )
	{
		dw_log( "v50: OUT%d %04X <- %0*X (at %04X:%04X)\n", size * 8, port, size * 2, v,
				v50_cpu.prev_cs, v50_cpu.prev_ip );
	}
	else
	{
		dw_log( "v50: IN%d %04X (at %04X:%04X)\n", size * 8, port, v50_cpu.prev_cs, v50_cpu.prev_ip );
	}
}

/**
  * @brief  Word input.
  *
  * @param  port The port.
  *
  * @retval The word.
  */
static uint16_t v50_in16( uint16_t port )
{
	if ( port >= 0x3000 && port < 0x3200 )
	{
		return asic_v50_in( port );
	}
	if ( port >= 0x2000 && port < 0x2020 )  // SCSI controllers: byte-wide, on D7-D0
	{
		return wd_read( ( port >> 4 ) & 1, port & 0xF );
	}
	v50_unknown( port, 0, 0, 2 );
	return 0xFFFF;
}

/**
  * @brief  Word output.
  *
  * @param  port The port.
  * @param  v The word.
  *
  * @retval None.
  */
static void v50_out16( uint16_t port, uint16_t v )
{
	if ( port >= 0x3000 && port < 0x3200 )
	{
		asic_v50_out( port, v );
		return;
	}
	if ( port >= 0x2000 && port < 0x2020 )
	{
		wd_write( ( port >> 4 ) & 1, port & 0xF, (uint8_t)v );
		return;
	}
	v50_unknown( port, 1, v, 2 );
}

/**
  * @brief  Byte input: the ASIC registers are word-wide, a byte read takes one lane.
  *
  * @param  port The port.
  *
  * @retval The byte.
  */
static uint8_t v50_in8( uint16_t port )
{
	if ( port >= 0x3000 && port < 0x3200 )
	{
		return (uint8_t)( asic_v50_in( (uint16_t)( port & ~1u ) ) >> ( ( port & 1 ) * 8 ) );
	}
	if ( port >= 0x2000 && port < 0x2020 )
	{
		return wd_read( ( port >> 4 ) & 1, port & 0xF );
	}
	v50_unknown( port, 0, 0, 1 );
	return 0xFF;
}

/**
  * @brief  Byte output.
  *
  * @param  port The port.
  * @param  v The byte.
  *
  * @retval None.
  */
static void v50_out8( uint16_t port, uint8_t v )
{
	if ( port >= 0x3000 && port < 0x3200 )
	{
		asic_v50_out( (uint16_t)( port & ~1u ), v );
		return;
	}
	if ( port >= 0x2000 && port < 0x2020 )
	{
		wd_write( ( port >> 4 ) & 1, port & 0xF, v );
		return;
	}
	if ( port >= 0xFFF0 )  // on-chip system registers (refresh, wait states): no effect here
	{
		return;
	}
	v50_unknown( port, 1, v, 1 );
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Power-on: clear memory, hold the CPU.
  *
  * @retval None.
  */
void v50_power_on( void )
{
	memset( v50_mem, 0, sizeof v50_mem );
	memset( v50_logged, 0, sizeof v50_logged );
	memset( &v50_cpu, 0, sizeof v50_cpu );
	v50_cpu.mem = v50_mem;
	v50_cpu.in8 = v50_in8;
	v50_cpu.in16 = v50_in16;
	v50_cpu.out8 = v50_out8;
	v50_cpu.out16 = v50_out16;
	v50_cpu.irq_pending = asic_v50_irq;
	v50_cpu.irq_ack = asic_v50_ack;
	v30_reset( &v50_cpu );
	wd_power_on();
	v50_was_running = 0;
	v50_frac = 0;
}

/**
  * @brief  Run the V50 for the time the 68306 just ran.
  *
  * @param  cycles_68k 68306 cycles elapsed.
  *
  * @retval None.
  */
void v50_run( int cycles_68k )
{
	int running = asic_v50_running(), n;

	if ( running && !v50_was_running )
	{
		v30_reset( &v50_cpu );
		dw_log( "v50: released, starting at FFFF:0000\n" );
	}
	v50_was_running = running;
	if ( !running )
	{
		return;
	}
	v50_frac += (double)cycles_68k * V50_HZ / DW_CPU_HZ;
	n = (int)v50_frac;
	v50_frac -= n;
	v50_frac -= v30_run( &v50_cpu, n ) - n;  // carry the overshoot of the last instruction
	wd_tick( n );
}

/**
  * @brief  Where the V50 is: CS:IP of the instruction being executed.
  *
  * @retval CS << 16 | IP.
  */
uint32_t v50_where( void )
{
	return (uint32_t)v50_cpu.prev_cs << 16 | v50_cpu.prev_ip;
}

/**
  * @brief  One-line CPU state for logs.
  *
  * @param  out Receives the text.
  * @param  n Room in out.
  *
  * @retval None.
  */
void v50_state( char *out, int n )
{
	v30_state( &v50_cpu, out, n );
}
