//  Description:
//
//		sim68306.h -- the MC68306's system registers (FFFFC0-FFFFFF).
//
//		Layout as the Darwin firmware uses it (ROM 001160, 04D612, 04D690):
//
//		  FFFFC0-DF  chip selects CS0-CS7, 32 bits each (B, C, D, E, F
//		             regions, ROM); recorded, the memory map is fixed
//		  FFFFE0-E7  DRAM bank 0 / 1 setup
//		  FFFFF0     port A data out    FFFFF1  port B data out
//		  FFFFF2     port A direction   FFFFF3  port B direction
//		  FFFFF4     port A pins        FFFFF5  port B pins
//		  FFFFFA-FB  interrupt control: for each external level n (1, 2, 3,
//		             5, 6, 7) bit 7+n enables it and bit n-1 makes it
//		             autovectored. Boot value 4545: levels 1, 3, 7 on and
//		             autovectored; the debug DUART adds 2000 (level 6,
//		             vectored), option cards add 0202 (L2) and 1010 (L5).
//		  FFFFFC-FE  bus timeout / system control bytes, recorded
//
//		Level 4 has no pin: it is the on-chip DUART, always enabled and
//		vectored by its IVR.
//

#ifndef SIM68306_H
#define SIM68306_H

#include <stdint.h>

extern uint8_t sim_port_pins[2];  // levels driven onto port A / B input pins

/**
  * @brief  Power-on reset of the system registers.
  *
  * @retval None.
  */
void sim_reset( void );

/**
  * @brief  Read a system register byte.
  *
  * @param  a Address, FFFFC0-FFFFFF (24-bit).
  *
  * @retval The byte.
  */
uint8_t sim_read8( uint32_t a );

/**
  * @brief  Write a system register byte.
  *
  * @param  a Address, FFFFC0-FFFFFF (24-bit).
  * @param  v The byte.
  *
  * @retval None.
  */
void sim_write8( uint32_t a, uint8_t v );

/**
  * @brief  Is an external interrupt level enabled in the interrupt control register?
  *
  * @param  level 1-7.
  *
  * @retval 1 enabled (level 4, the on-chip DUART, always is), 0 masked.
  */
int sim_level_enabled( int level );

/**
  * @brief  Is an external interrupt level autovectored?
  *
  * @param  level 1-7.
  *
  * @retval 1 autovectored, 0 the device supplies a vector.
  */
int sim_autovector( int level );

/**
  * @brief  The value a port drives on its output pins.
  *
  * @param  port 0 = A, 1 = B.
  *
  * @retval Data register, masked by direction (1 = output).
  */
uint8_t sim_port_out( int port );

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
unsigned sim_port_falls( int port, int bit );

#endif
