//  Description:
//
//		midi_sys.h -- the host MIDI system under midi.c: one backend per system.
//
//		  midi_win.c   Windows, winmm
//		  midi_alsa.c  Linux, the ALSA sequencer
//		  midi_mac.c   macOS, CoreMIDI
//
//		midi.c keeps what is common: the receive ring, the parser that turns
//		the Darwin's transmitted bytes into whole messages, and finding a
//		device by name. A backend lists devices, opens one input and one
//		output, sends whole messages, and hands every byte it receives, on
//		whatever thread, to midi_sys_received.
//

#ifndef MIDI_SYS_H
#define MIDI_SYS_H

#include "midi.h"

#include <stdint.h>

/**
  * @brief  Number of host MIDI devices.
  *
  * @param  out 1 for outputs (the Darwin sends to them), 0 for inputs.
  *
  * @retval The count.
  */
int midi_sys_count( int out );

/**
  * @brief  A device's name.
  *
  * @param  out 1 for outputs.
  * @param  i Device index.
  * @param  name Receives the name.
  *
  * @retval 0 found, -1 no such device.
  */
int midi_sys_name( int out, int i, char name[MIDI_NAME] );

/**
  * @brief  Open a device (the one of that direction already open is closed first).
  *
  * @param  out 1 for an output.
  * @param  i Device index.
  *
  * @retval 0 open, -1 failed.
  */
int midi_sys_open( int out, int i );

/**
  * @brief  Close both directions.
  *
  * @retval None.
  */
void midi_sys_close( void );

/**
  * @brief  Is a direction open?
  *
  * @param  out 1 for the output.
  *
  * @retval 1 open.
  */
int midi_sys_is_open( int out );

/**
  * @brief  Send one whole message: a short message (1-3 bytes) or SysEx (F0 ... F7).
  *
  * @param  d The bytes.
  * @param  n How many.
  *
  * @retval None.
  */
void midi_sys_send( const uint8_t *d, int n );

/**
  * @brief  The emulator thread is about to take received bytes (winmm hands its SysEx
  *         buffers back here). May do nothing.
  *
  * @retval None.
  */
void midi_sys_poll( void );

/**
  * @brief  A byte from the input device: for the backends, on any thread (midi.c).
  *
  * @param  b The byte.
  *
  * @retval None.
  */
void midi_sys_received( uint8_t b );

#endif
