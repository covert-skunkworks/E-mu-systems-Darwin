//  Description:
//
//		panel.h -- the Darwin's front panel, emulated at its link protocol.
//
//		The front panel has its own processor. It is connected to channel A
//		of the 68306 DUART (the firmware calls it the "remote") and speaks
//		E-mu system exclusive without an F7 terminator: every message starts
//		with F0 18 0B <device> and ends where the next F0 begins.
//
//		  68306 -> panel
//		    00 01 n            LCD contrast 0-15
//		    00 02 i lo hi      link timeouts (ms): 0 = byte, 1 = reply
//		    00 06              (sent once at start)
//		    00 08              request the panel's 5-byte version
//		    01 <values>        a graphics command (below)
//		    02 g i s           LED i of group g on (s 1) / off (0);
//		                       groups 4 (6 LEDs), 5 (8), 6 (4)
//		    03 g b0..b7        8 bytes for group g (meters)
//
//		  panel -> 68306
//		    00 09 b0..b4       the version, answer to 00 08
//		    04 g i s           key i of group g pressed (1) / released (0)
//		    05 x lo hi v..     encoder (data wheel)
//		    06 x v             shuttle position, signed 7-bit
//
//		Graphics commands are lists of 14-bit values, each sent as two 7-bit
//		bytes (low first); the first value is the command. They are the
//		firmware's own graphics library calls (ROM 084000-087FFF), made on a
//		screen the library marks as remote. The ROM also contains the
//		receiving side (ROM 085892 decodes one command and calls the local
//		library), so the panel is emulated by running that code on a second,
//		private 68000 context with its own RAM, where the screen is local
//		and draws into a 240 x 64 bitmap (1 bit per pixel, MSB = left). Font
//		downloads (command 14: id, length, bytes) are registered natively.
//

#ifndef PANEL_H
#define PANEL_H

#include <stdint.h>

#define PNL_W     240
#define PNL_H     64
#define PNL_ROWB  ( PNL_W / 8 )

/**
  * @brief  Power-on: clear the link and the display, start the panel's processor.
  *
  * @retval None.
  */
void pnl_power_on( void );

/**
  * @brief  Another firmware was loaded: find its routines again at the next power-on.
  *
  * @retval None.
  */
void pnl_rom_changed( void );

/**
  * @brief  A byte the 68306 sends on the panel link (DUART channel A).
  *
  * Called from the DUART: it is only queued here.
  *
  * @param  b The byte.
  *
  * @retval None.
  */
void pnl_rx( uint8_t b );

/**
  * @brief  Process queued link traffic (outside the 68306's execution).
  *
  * @param  cycles 68306 cycles since the last call.
  *
  * @retval None.
  */
void pnl_poll( int cycles );

/**
  * @brief  Copy the display bitmap.
  *
  * @param  out Receives PNL_ROWB * PNL_H bytes, 1 = dark pixel.
  *
  * @retval None.
  */
void pnl_bitmap( uint8_t *out );

/**
  * @brief  LED state.
  *
  * @param  group LED group 0-7.
  * @param  index LED index 0-15.
  *
  * @retval 0 dark, 1 lit, 2 and up flashing (4 an armed track, 2 / 5 during record start).
  */
int pnl_led( int group, int index );

/**
  * @brief  The LCD contrast the firmware set (0-15, 8 = middle).
  *
  * @retval Contrast.
  */
int pnl_contrast( void );

/**
  * @brief  The 8 bytes last sent for a "03" group (meters).
  *
  * @param  group Group number.
  * @param  out Receives 8 bytes.
  *
  * @retval None.
  */
void pnl_group_bytes( int group, uint8_t out[8] );

/**
  * @brief  A panel key changed.
  *
  * @param  group Key group 0-6.
  * @param  index Key index in the group.
  * @param  down 1 pressed, 0 released.
  *
  * @retval None.
  */
void pnl_key( int group, int index, int down );

/**
  * @brief  The data wheel turned.
  *
  * @param  steps Steps, positive = clockwise.
  *
  * @retval None.
  */
void pnl_wheel( int steps );

/**
  * @brief  Is the panel's processor in the middle of a call (memory routing)?
  *
  * @retval 1 while the panel context runs.
  */
int pnl_active( void );

/**
  * @brief  Instruction hook while the panel context runs.
  *
  * @param  pc The instruction's address.
  *
  * @retval None.
  */
void pnl_hook( uint32_t pc );

/**
  * @brief  Panel memory read (while pnl_active()).
  *
  * @param  a Address (24-bit).
  *
  * @retval The byte.
  */
uint8_t pnl_read8( uint32_t a );

/**
  * @brief  Panel memory write (while pnl_active()).
  *
  * @param  a Address (24-bit).
  * @param  v The byte.
  *
  * @retval None.
  */
void pnl_write8( uint32_t a, uint8_t v );

#endif
