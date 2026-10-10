//  Description:
//
//		midi.h -- the Darwin's MIDI port (68306 DUART channel B) on host MIDI devices.
//

#ifndef MIDI_H
#define MIDI_H

#include <stdint.h>

#define MIDI_NAME 64

/**
  * @brief  Open the host devices by name ("" = none); closes what was open.
  *
  * @param  in_name MIDI input device.
  * @param  out_name MIDI output device.
  *
  * @retval 0 all named devices open, -1 one of them did not open (it stays closed).
  */
int midi_open( const char *in_name, const char *out_name );

/**
  * @brief  Close the host devices.
  *
  * @retval None.
  */
void midi_close( void );

/**
  * @brief  A byte the Darwin transmits on MIDI out.
  *
  * Collected into messages (running status, real-time bytes between, SysEx)
  * and sent to the host output device.
  *
  * @param  b The byte.
  *
  * @retval None.
  */
void midi_out_byte( uint8_t b );

/**
  * @brief  Take the next byte received from the host input device.
  *
  * @param  b Receives the byte.
  *
  * @retval 1 a byte, 0 none waiting.
  */
int midi_in_byte( uint8_t *b );

/**
  * @brief  Number of host MIDI input / output devices.
  *
  * @param  out 1 for outputs.
  *
  * @retval The count.
  */
int midi_dev_count( int out );

/**
  * @brief  Name of a host MIDI device.
  *
  * @param  out 1 for outputs.
  * @param  i Device index.
  * @param  name Receives the name (MIDI_NAME bytes).
  *
  * @retval 0 found, -1 no such device.
  */
int midi_dev_name( int out, int i, char name[MIDI_NAME] );

#endif
