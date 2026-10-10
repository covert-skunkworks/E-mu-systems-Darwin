//  Description:
//
//		audio.h -- the Darwin's converters on the host's sound devices.
//
//		The ASIC hands over one frame per sample period (asic.c): the twelve
//		outputs out, the six inputs back. Where each goes on the host is the
//		routing of the settings window (settings.h in1-10, out1-12).
//

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#define AU_RATE 44100
#define AU_OUTS 12  // track outputs 1-8, phones L / R, S/PDIF out L / R (the last four: the mix)
#define AU_INS  10  // analog inputs 1-8, S/PDIF L, R

/**
  * @brief  Open the host devices named in the settings (playback, recording).
  *
  * @retval 0 playing, -1 no playback device (the machine runs silent).
  */
int au_open( void );

/**
  * @brief  Close the host devices.
  *
  * @retval None.
  */
void au_close( void );

/**
  * @brief  One sample period: the outputs go to the host, the inputs come back.
  *
  * @param  out The twelve outputs.
  * @param  in Receives the six inputs (silence when there is no input).
  *
  * @retval None.
  */
void au_frame( const int16_t out[AU_OUTS], int16_t in[AU_INS] );

/**
  * @brief  Set the headphone volume knob (an analog pot on the real unit, after the mix).
  *
  * @param  pos Knob position 0-1: 0 off, then -48 dB to 0 dB along a log taper.
  *
  * @retval None.
  */
void au_set_phones( float pos );

/**
  * @brief  The headphone gain the knob gives.
  *
  * @retval Linear gain 0-1.
  */
float au_phones_gain( void );

/**
  * @brief  Also write the mix outputs to a WAV file (-wav), for runs without a device.
  *
  * @param  path The file.
  *
  * @retval 0 open, -1 failed.
  */
int au_wav_out( const char *path );

/**
  * @brief  Feed the analog inputs from a WAV file instead of the device (-wavin).
  *
  * 16-bit PCM; channel 1 feeds input 1, channel 2 input 2.
  *
  * @param  path The file.
  *
  * @retval 0 open, -1 failed or not 16-bit PCM.
  */
int au_wav_in( const char *path );

/**
  * @brief  Finish the WAV files (sizes in their headers).
  *
  * @retval None.
  */
void au_wav_close( void );

#endif
