//  Description:
//
//		machine.c -- the Darwin main board: memory map, interrupts, devices.
//
//		Absent hardware answers with a bus error, as the real bus timeout
//		does: the firmware probes the debug DUART (B00001), the DSP card
//		(E00000) and the option cards (F00000) under a bus-error trap (ROM
//		04D8F2 arms it, 000252 is the handler) and records what is fitted.
//		Other unmapped accesses are logged and read as FF, so unknown
//		hardware shows up in the log instead of crashing the firmware.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "machine.h"
#include "asic.h"
#include "darwin.h"
#include "rtc.h"
#include "sim68306.h"
#include "panel.h"
#include "v50.h"

#include <string.h>

#include "m68k.h"

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define MC_PAGES      4096  // unmapped-access log: 4 KB pages of the 16 MB space
#define MC_LOG_PER_PG 8

// the decoded windows; the rest of each chip select is logged as unmapped, so unknown
// devices sharing a select show up
#define MC_IN_DBG( a )  ( ( a ) >= 0xB00000 && ( a ) < 0xB00020 )
#define MC_IN_ASIC( a ) ( ( a ) >= 0xC00000 && ( a ) < 0xC00200 )
#define MC_IN_RTC( a )  ( ( a ) >= 0xD00000 && ( a ) < 0xD00004 )

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

uint8_t  dw_rom[DW_ROM_SIZE];
uint8_t  dw_ram[DW_RAM_SIZE];
duart    mc_duart = { .name = "68306", .xtal = 3000000.0, .ip2_hz = 3000000.0, .cpu_hz = DW_CPU_HZ };
duart    mc_dbg_duart = { .name = "debug", .xtal = 3686400.0, .ip2_hz = 3686400.0, .cpu_hz = DW_CPU_HZ };
int      mc_dbg_fitted = 1;
uint32_t mc_wlog_lo, mc_wlog_hi;
int      mc_wlog_n;

static unsigned mc_unmapped[MC_PAGES];
static int      mc_level;

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void mc_unmapped_access( uint32_t a, int size, int write, uint32_t v );
static int mc_absent( uint32_t a );
static uint8_t mc_read8( uint32_t a );
static uint16_t mc_read16( uint32_t a );
static void mc_write8( uint32_t a, uint8_t v );
static void mc_write16( uint32_t a, uint16_t v );
static void mc_io_trace( uint32_t a, int size, uint32_t v );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  -ioall: log a 68306 write to a peripheral other than the ASIC (which logs its own).
  *
  * @param  a Address.
  * @param  size 1 or 2 bytes.
  * @param  v Value written.
  *
  * @retval None.
  */
static void mc_io_trace( uint32_t a, int size, uint32_t v )
{
	if ( dw_trace_asic && dw_cycles >= dw_trace_asic && !MC_IN_ASIC( a ) )
	{
		dw_log( "%10.6f 68k W %06X %0*X  %06X\n", (double)dw_cycles / DW_CPU_HZ, (unsigned)a, size * 2,
				(unsigned)v, dw_pc() );
	}
}

static void mc_unmapped_access( uint32_t a, int size, int write, uint32_t v )
{
	unsigned *n = &mc_unmapped[( a >> 12 ) & ( MC_PAGES - 1 )];

	if ( ( *n )++ < MC_LOG_PER_PG || dw_log_io )
	{
		if ( write )
		{
			dw_log( "unmapped write%d %06X <- %0*X (PC %06X)\n", size * 8, (unsigned)a, size * 2,
					(unsigned)v, dw_pc() );
		}
		else
		{
			dw_log( "unmapped read%d %06X (PC %06X)\n", size * 8, (unsigned)a, dw_pc() );
		}
	}
}

/**
  * @brief  Is the address in the range of a probed board that is not fitted?
  *
  * @param  a Address.
  *
  * @retval 1 absent (bus error), 0 not.
  */
static int mc_absent( uint32_t a )
{
	if ( a >= 0xB00000 && a < 0xC00000 )
	{
		return !mc_dbg_fitted;
	}
	if ( a >= 0xE00000 && a < 0xFFF000 )  // DSP card, option cards
	{
		return 1;
	}
	return 0;
}

/**
  * @brief  Byte read through the memory map.
  *
  * @param  a Address (24-bit).
  *
  * @retval The byte.
  */
static uint8_t mc_read8( uint32_t a )
{
	uint8_t v;

	if ( pnl_active() )  // the front panel's processor is running (panel.c)
	{
		return pnl_read8( a );
	}
	if ( a < DW_ROM_SIZE )
	{
		return dw_rom[a];
	}
	if ( a - DW_RAM_BASE < DW_RAM_SIZE )
	{
		return dw_ram[a - DW_RAM_BASE];
	}
	// reads that clear an interrupt source (receive data, RTC register C) must update the CPU's
	// interrupt level at once, or it takes a stale interrupt with nothing to vector it
	if ( MC_IN_DBG( a ) && mc_dbg_fitted )
	{
		v = ( a & 1 ) ? duart_read( &mc_dbg_duart, (int)( ( a >> 1 ) & 15 ) ) : 0xFF;
		dw_irq_update();
		return v;
	}
	if ( MC_IN_ASIC( a ) )
	{
		return (uint8_t)( asic_read16( a & ~1u ) >> ( ( a & 1 ) ? 0 : 8 ) );
	}
	if ( MC_IN_RTC( a ) )
	{
		v = ( a & 3 ) == 1 ? rtc_read() : 0xFF;
		dw_irq_update();
		return v;
	}
	if ( a >= 0xFFF7E0 && a < 0xFFF800 )
	{
		v = ( a & 1 ) ? duart_read( &mc_duart, (int)( ( a >> 1 ) & 15 ) ) : 0xFF;
		dw_irq_update();
		return v;
	}
	if ( a >= 0xFFFFC0 )
	{
		return sim_read8( a );
	}
	if ( mc_absent( a ) )
	{
		dw_log( "bus error: read %06X (PC %06X)\n", (unsigned)a, dw_pc() );
		m68k_bus_error_at( a, 0 );
	}
	mc_unmapped_access( a, 1, 0, 0 );
	return 0xFF;
}

/**
  * @brief  Word read through the memory map.
  *
  * @param  a Address (24-bit, even).
  *
  * @retval The word.
  */
static uint16_t mc_read16( uint32_t a )
{
	if ( pnl_active() )
	{
		return (uint16_t)( ( pnl_read8( a ) << 8 ) | pnl_read8( a + 1 ) );
	}
	if ( a < DW_ROM_SIZE - 1 )
	{
		return (uint16_t)( ( dw_rom[a] << 8 ) | dw_rom[a + 1] );
	}
	if ( a - DW_RAM_BASE < DW_RAM_SIZE - 1 )
	{
		return (uint16_t)( ( dw_ram[a - DW_RAM_BASE] << 8 ) | dw_ram[a - DW_RAM_BASE + 1] );
	}
	if ( MC_IN_ASIC( a ) )
	{
		return asic_read16( a );
	}
	if ( MC_IN_RTC( a ) )
	{
		uint16_t w = ( a & 2 ) ? 0xFFFF : (uint16_t)( 0xFF00 | rtc_read() );

		dw_irq_update();  // reading register C acknowledges the tick
		return w;
	}
	return (uint16_t)( ( mc_read8( a ) << 8 ) | mc_read8( a + 1 ) );
}

/**
  * @brief  Byte write through the memory map.
  *
  * @param  a Address (24-bit).
  * @param  v The byte.
  *
  * @retval None.
  */
static void mc_write8( uint32_t a, uint8_t v )
{
	uint32_t pc;

	if ( pnl_active() )
	{
		pnl_write8( a, v );
		return;
	}
	if ( mc_wlog_n > 0 )  // -wlog: who writes where
	{
		pc = dw_pc();
		if ( pc >= mc_wlog_lo && pc <= mc_wlog_hi )
		{
			mc_wlog_n--;
			dw_log( "write %06X <- %02X (PC %06X)\n", (unsigned)a, v, pc );
		}
	}
	if ( a - DW_RAM_BASE < DW_RAM_SIZE )
	{
		dw_ram[a - DW_RAM_BASE] = v;
		return;
	}
	if ( a < DW_ROM_SIZE )
	{
		mc_unmapped_access( a, 1, 1, v );  // EPROM: nothing happens
		return;
	}
	mc_io_trace( a, 1, v );
	if ( MC_IN_DBG( a ) && mc_dbg_fitted )
	{
		if ( a & 1 )
		{
			duart_write( &mc_dbg_duart, (int)( ( a >> 1 ) & 15 ), v );
			dw_irq_update();
		}
		return;
	}
	if ( MC_IN_ASIC( a ) )
	{
		// a byte write reaches the ASIC as a word with the other lane as read back
		uint16_t w = asic_read16( a & ~1u );

		w = ( a & 1 ) ? (uint16_t)( ( w & 0xFF00 ) | v ) : (uint16_t)( ( w & 0x00FF ) | ( v << 8 ) );
		asic_write16( a & ~1u, w );
		return;
	}
	if ( MC_IN_RTC( a ) )
	{
		if ( ( a & 3 ) == 3 )
		{
			rtc_select( v );
		}
		else if ( ( a & 3 ) == 1 )
		{
			rtc_write( v );
		}
		return;
	}
	if ( a >= 0xFFF7E0 && a < 0xFFF800 )
	{
		if ( a & 1 )
		{
			duart_write( &mc_duart, (int)( ( a >> 1 ) & 15 ), v );
			dw_irq_update();
		}
		return;
	}
	if ( a >= 0xFFFFC0 )
	{
		sim_write8( a, v );
		return;
	}
	if ( mc_absent( a ) )
	{
		dw_log( "bus error: write %06X (PC %06X)\n", (unsigned)a, dw_pc() );
		m68k_bus_error_at( a, 1 );
	}
	mc_unmapped_access( a, 1, 1, v );
}

/**
  * @brief  Word write through the memory map.
  *
  * @param  a Address (24-bit, even).
  * @param  v The word.
  *
  * @retval None.
  */
static void mc_write16( uint32_t a, uint16_t v )
{
	if ( pnl_active() )
	{
		pnl_write8( a, (uint8_t)( v >> 8 ) );
		pnl_write8( a + 1, (uint8_t)v );
		return;
	}
	if ( a - DW_RAM_BASE < DW_RAM_SIZE - 1 )
	{
		dw_ram[a - DW_RAM_BASE] = (uint8_t)( v >> 8 );
		dw_ram[a - DW_RAM_BASE + 1] = (uint8_t)v;
		return;
	}
	mc_io_trace( a, 2, v );
	if ( MC_IN_ASIC( a ) )
	{
		asic_write16( a, v );
		return;
	}
	if ( MC_IN_RTC( a ) )
	{
		if ( a & 2 )
		{
			rtc_select( (uint8_t)v );
		}
		else
		{
			rtc_write( (uint8_t)v );
		}
		return;
	}
	mc_write8( a, (uint8_t)( v >> 8 ) );
	mc_write8( a + 1, (uint8_t)v );
}

//------------------------------------------------------------------------------
//--------------------------- Musashi callbacks --------------------------------
//------------------------------------------------------------------------------

unsigned int m68k_read_memory_8( unsigned int a ) { return mc_read8( a & 0xFFFFFF ); }
unsigned int m68k_read_memory_16( unsigned int a ) { return mc_read16( a & 0xFFFFFF ); }
unsigned int m68k_read_memory_32( unsigned int a )
{
	return ( (unsigned)mc_read16( a & 0xFFFFFF ) << 16 ) | mc_read16( ( a + 2 ) & 0xFFFFFF );
}
void m68k_write_memory_8( unsigned int a, unsigned int v ) { mc_write8( a & 0xFFFFFF, (uint8_t)v ); }
void m68k_write_memory_16( unsigned int a, unsigned int v ) { mc_write16( a & 0xFFFFFF, (uint16_t)v ); }
void m68k_write_memory_32( unsigned int a, unsigned int v )
{
	mc_write16( a & 0xFFFFFF, (uint16_t)( v >> 16 ) );
	mc_write16( ( a + 2 ) & 0xFFFFFF, (uint16_t)v );
}
unsigned int m68k_read_immediate_16( unsigned int a ) { return mc_read16( a & 0xFFFFFF ); }
unsigned int m68k_read_immediate_32( unsigned int a ) { return m68k_read_memory_32( a ); }
unsigned int m68k_read_pcrelative_8( unsigned int a ) { return mc_read8( a & 0xFFFFFF ); }
unsigned int m68k_read_pcrelative_16( unsigned int a ) { return mc_read16( a & 0xFFFFFF ); }
unsigned int m68k_read_pcrelative_32( unsigned int a ) { return m68k_read_memory_32( a ); }
unsigned int m68k_read_disassembler_8( unsigned int a ) { return mc_peek8( a & 0xFFFFFF ); }
unsigned int m68k_read_disassembler_16( unsigned int a )
{
	return ( (unsigned)mc_peek8( a & 0xFFFFFF ) << 8 ) | mc_peek8( ( a + 1 ) & 0xFFFFFF );
}
unsigned int m68k_read_disassembler_32( unsigned int a )
{
	return ( m68k_read_disassembler_16( a ) << 16 ) | m68k_read_disassembler_16( a + 2 );
}

/**
  * @brief  Interrupt acknowledge: the vector for a level.
  *
  * @param  level The level being acknowledged.
  *
  * @retval A vector number, or M68K_INT_ACK_AUTOVECTOR.
  */
int emu_int_ack( int level )
{
	if ( level == 4 && duart_irq( &mc_duart ) )
	{
		return mc_duart.ivr;
	}
	if ( level == 6 && mc_dbg_fitted && duart_irq( &mc_dbg_duart ) )
	{
		return mc_dbg_duart.ivr;
	}
	if ( sim_autovector( level ) )
	{
		return M68K_INT_ACK_AUTOVECTOR;
	}
	dw_log( "IACK level %d: no vectoring source (PC %06X)\n", level, dw_pc() );
	return M68K_INT_ACK_SPURIOUS;
}

void emu_reset_instr( void ) { dw_log( "RESET instruction at %06X\n", dw_pc() ); }
int emu_illg( int opcode ) { (void)opcode; return 0; }
void emu_cf_movec( int reg, unsigned int value ) { (void)reg; (void)value; }

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Recompute the CPU's interrupt level from all sources.
  *
  * @retval None.
  */
void dw_irq_update( void )
{
	int level = 0;

	if ( rtc_irq() && sim_level_enabled( 1 ) )
	{
		level = 1;
	}
	if ( asic_irq() && sim_level_enabled( 3 ) )
	{
		level = 3;
	}
	if ( duart_irq( &mc_duart ) )
	{
		level = 4;
	}
	if ( mc_dbg_fitted && duart_irq( &mc_dbg_duart ) && sim_level_enabled( 6 ) )
	{
		level = 6;
	}
	if ( level != mc_level )
	{
		mc_level = level;
		m68k_set_irq( (unsigned)level );
	}
}

/**
  * @brief  Cold start: reset every chip and the CPU (ROM and NVRAM are kept).
  *
  * @retval None.
  */
void mc_power_on( void )
{
	memset( dw_ram, 0, sizeof dw_ram );
	memset( mc_unmapped, 0, sizeof mc_unmapped );
	sim_reset();
	asic_reset( v50_mem );
	v50_power_on();
	duart_reset( &mc_duart );
	duart_reset( &mc_dbg_duart );
	mc_duart.ip = 0xFF;
	mc_dbg_duart.ip = 0xFF;
	mc_level = 0;
	m68k_init();
	m68k_set_cpu_type( M68K_CPU_TYPE_68000 );
	m68k_pulse_reset();
	m68k_set_irq( 0 );
	pnl_power_on();  // after the CPU core is set up: the panel runs on a copy of it
}

/**
  * @brief  Run the CPU and the chips for a slice of emulated time.
  *
  * @param  cycles CPU cycles to run.
  *
  * @retval Cycles actually run.
  */
int mc_run( int cycles )
{
	int ran = m68k_execute( cycles );

	dw_cycles += (uint64_t)ran;
	duart_tick( &mc_duart, ran );
	if ( mc_dbg_fitted )
	{
		duart_tick( &mc_dbg_duart, ran );
	}
	rtc_tick( ran, DW_CPU_HZ );
	asic_tick( ran );
	v50_run( ran );
	pnl_poll( ran );
	dw_irq_update();
	return ran;
}

/**
  * @brief  Read a byte without side effects (debugger, disassembler).
  *
  * @param  a Address (24-bit).
  *
  * @retval The byte, FF for I/O.
  */
uint8_t mc_peek8( uint32_t a )
{
	if ( a < DW_ROM_SIZE )
	{
		return dw_rom[a];
	}
	if ( a - DW_RAM_BASE < DW_RAM_SIZE )
	{
		return dw_ram[a - DW_RAM_BASE];
	}
	return 0xFF;
}

/**
  * @brief  Log a summary of the unmapped addresses the firmware touched.
  *
  * @retval None.
  */
void mc_unmapped_report( void )
{
	int p;

	for ( p = 0; p < MC_PAGES; p++ )
	{
		if ( mc_unmapped[p] )
		{
			dw_log( "unmapped page %06X: %u accesses\n", (unsigned)p << 12, mc_unmapped[p] );
		}
	}
}
