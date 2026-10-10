//  Description:
//
//		sim68306.c -- the MC68306's system registers (FFFFC0-FFFFFF).
//
//		A register file with the few behaviours the firmware depends on:
//		port pin reads and the interrupt control word. The chip-select and
//		DRAM registers only configure decoding the emulator does with a
//		fixed map, so they are stored and logged.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "sim68306.h"
#include "darwin.h"

#include <string.h>

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static uint8_t sim_reg[64];  // FFFFC0-FFFFFF
uint8_t        sim_port_pins[2] = { 0xFF, 0xFF };
static unsigned sim_port_falls_n[2][8];  // output pins gone low

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static uint16_t sim_icr( void );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  The interrupt control word (FFFFFA-FB).
  *
  * @retval ICR.
  */
static uint16_t sim_icr( void ) { return (uint16_t)( ( sim_reg[0x3A] << 8 ) | sim_reg[0x3B] ); }

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Power-on reset of the system registers.
  *
  * @retval None.
  */
void sim_reset( void )
{
	memset( sim_reg, 0, sizeof sim_reg );
}

/**
  * @brief  Read a system register byte.
  *
  * @param  a Address, FFFFC0-FFFFFF (24-bit).
  *
  * @retval The byte.
  */
uint8_t sim_read8( uint32_t a )
{
	int r = (int)( a & 0x3F ), port;

	if ( r == 0x34 || r == 0x35 )  // pins: outputs read back, inputs from outside
	{
		port = r - 0x34;
		return (uint8_t)( ( sim_reg[0x30 + port] & sim_reg[0x32 + port] ) |
						  ( sim_port_pins[port] & ~sim_reg[0x32 + port] ) );
	}
	return sim_reg[r];
}

/**
  * @brief  Write a system register byte.
  *
  * @param  a Address, FFFFC0-FFFFFF (24-bit).
  * @param  v The byte.
  *
  * @retval None.
  */
void sim_write8( uint32_t a, uint8_t v )
{
	int r = (int)( a & 0x3F ), bit;
	uint8_t old, fell;

	if ( r < 0x30 && dw_log_io )
	{
		dw_log( "sim: %06X <- %02X (PC %06X)\n", (unsigned)a, v, dw_pc() );
	}
	if ( r == 0x30 || r == 0x31 || r == 0x32 || r == 0x33 )  // a port's data or direction: count pins going low
	{
		old = sim_port_out( r & 1 );
		sim_reg[r] = v;
		fell = (uint8_t)( old & ~sim_port_out( r & 1 ) );
		for ( bit = 0; bit < 8; bit++ )
		{
			sim_port_falls_n[r & 1][bit] += ( fell >> bit ) & 1;
		}
		return;
	}
	sim_reg[r] = v;
	if ( r == 0x3A || r == 0x3B )
	{
		dw_irq_update();
	}
}

/**
  * @brief  Is an external interrupt level enabled in the interrupt control register?
  *
  * @param  level 1-7.
  *
  * @retval 1 enabled (level 4, the on-chip DUART, always is), 0 masked.
  */
int sim_level_enabled( int level )
{
	if ( level == 4 )
	{
		return 1;
	}
	if ( level < 1 || level > 7 )
	{
		return 0;
	}
	return ( sim_icr() >> ( 7 + level ) ) & 1;
}

/**
  * @brief  Is an external interrupt level autovectored?
  *
  * @param  level 1-7.
  *
  * @retval 1 autovectored, 0 the device supplies a vector.
  */
int sim_autovector( int level )
{
	if ( level == 4 || level < 1 || level > 7 )
	{
		return 0;
	}
	return ( sim_icr() >> ( level - 1 ) ) & 1;
}

/**
  * @brief  The value a port drives on its output pins.
  *
  * @param  port 0 = A, 1 = B.
  *
  * @retval Data register, masked by direction (1 = output).
  */
uint8_t sim_port_out( int port )
{
	port &= 1;
	return (uint8_t)( sim_reg[0x30 + port] & sim_reg[0x32 + port] );
}

/**
  * @brief  How often an output pin has gone low (for LEDs the firmware pulses).
  *
  * Port A bit 1 is the MIDI LED (active low; ROM 04D80C lights it for each
  * MIDI byte received, for 5 ms): a GUI sampling at its frame rate would
  * miss most pulses, a count does not.
  *
  * @param  port 0 = A, 1 = B.
  * @param  bit Pin 0-7.
  *
  * @retval High-to-low transitions since power-on.
  */
unsigned sim_port_falls( int port, int bit )
{
	return sim_port_falls_n[port & 1][bit & 7];
}
