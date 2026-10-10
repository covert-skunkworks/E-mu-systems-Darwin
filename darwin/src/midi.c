//  Description:
//
//		midi.c -- the Darwin's MIDI port (68306 DUART channel B) on host MIDI devices.
//
//		The host side is a backend per system (midi_sys.h: winmm, ALSA,
//		CoreMIDI); this file is the part they share.
//
//		In: the backend hands over received bytes on its own thread. They go
//		into a ring; the emulator thread takes them (midi_in_byte) into the
//		DUART's receiver, which delivers them at MIDI speed. SysEx matters
//		here: MIDI Machine Control, which drives the Darwin's transport, is
//		SysEx.
//
//		Out: the bytes the firmware transmits are collected into messages
//		(running status, real-time bytes allowed anywhere, SysEx up to
//		64 KB), and each whole message goes to the backend.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "midi.h"
#include "midi_sys.h"
#include "darwin.h"

#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define MIDI_RING  8192   // received bytes waiting for the DUART
#define MIDI_SXOUT 65535  // the longest SysEx sent (CoreMIDI's packet length is 16 bits)

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static SDL_SpinLock     midi_lock;
static uint8_t          midi_ring[MIDI_RING];
static unsigned         midi_head, midi_tail;   // written by the backend / taken by the emulator

// output parser
static uint8_t          midi_status, midi_msg[3];
static int              midi_need, midi_have;
static uint8_t          midi_sx[MIDI_SXOUT];
static int              midi_sx_len, midi_in_sx;

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static int midi_data_bytes( uint8_t st );
static void midi_send_short( uint8_t a, uint8_t b, uint8_t c, int n );
static int midi_find( int out, const char *name );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Data bytes that follow a status byte.
  *
  * @param  st The status byte.
  *
  * @retval 0-2.
  */
static int midi_data_bytes( uint8_t st )
{
	if ( st < 0xF0 )
	{
		return ( st & 0xE0 ) == 0xC0 ? 1 : 2;  // program change, channel pressure: 1
	}
	return st == 0xF1 || st == 0xF3 ? 1 : st == 0xF2 ? 2 : 0;
}

/**
  * @brief  Send a short message.
  *
  * @param  a Status.
  * @param  b First data byte.
  * @param  c Second data byte.
  * @param  n Bytes in the message, 1-3.
  *
  * @retval None.
  */
static void midi_send_short( uint8_t a, uint8_t b, uint8_t c, int n )
{
	uint8_t m[3];

	m[0] = a;
	m[1] = b;
	m[2] = c;
	if ( midi_sys_is_open( 1 ) )
	{
		midi_sys_send( m, n );
	}
}

/**
  * @brief  Find a host device by name.
  *
  * @param  out 1 for outputs.
  * @param  name The name.
  *
  * @retval Its index, -1 if not there.
  */
static int midi_find( int out, const char *name )
{
	char n[MIDI_NAME];
	int i;

	for ( i = 0; i < midi_sys_count( out ); i++ )
	{
		if ( midi_sys_name( out, i, n ) == 0 && !strcmp( n, name ) )
		{
			return i;
		}
	}
	return -1;
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  A byte from the input device: for the backends, on any thread.
  *
  * Dropped when the ring is full.
  *
  * @param  b The byte.
  *
  * @retval None.
  */
void midi_sys_received( uint8_t b )
{
	SDL_AtomicLock( &midi_lock );
	if ( midi_head - midi_tail < MIDI_RING )
	{
		midi_ring[midi_head++ % MIDI_RING] = b;
	}
	SDL_AtomicUnlock( &midi_lock );
}

/**
  * @brief  Open the host devices by name ("" = none); closes what was open.
  *
  * @param  in_name MIDI input device.
  * @param  out_name MIDI output device.
  *
  * @retval 0 all named devices open, -1 one of them did not open (it stays closed).
  */
int midi_open( const char *in_name, const char *out_name )
{
	int rc = 0, d;

	midi_close();
	if ( out_name[0] )
	{
		d = midi_find( 1, out_name );
		if ( d < 0 || midi_sys_open( 1, d ) != 0 )
		{
			dw_log( "midi: cannot open output %s\n", out_name );
			rc = -1;
		}
	}
	if ( in_name[0] )
	{
		d = midi_find( 0, in_name );
		if ( d < 0 || midi_sys_open( 0, d ) != 0 )
		{
			dw_log( "midi: cannot open input %s\n", in_name );
			rc = -1;
		}
	}
	if ( midi_sys_is_open( 0 ) || midi_sys_is_open( 1 ) )
	{
		dw_log( "midi: in %s, out %s\n", midi_sys_is_open( 0 ) ? in_name : "(none)",
				midi_sys_is_open( 1 ) ? out_name : "(none)" );
	}
	return rc;
}

/**
  * @brief  Close the host devices.
  *
  * @retval None.
  */
void midi_close( void )
{
	midi_sys_close();
	midi_status = 0;
	midi_in_sx = 0;
	SDL_AtomicLock( &midi_lock );
	midi_tail = midi_head;
	SDL_AtomicUnlock( &midi_lock );
}

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
void midi_out_byte( uint8_t b )
{
	if ( b >= 0xF8 )  // real time: on its own, anywhere
	{
		midi_send_short( b, 0, 0, 1 );
		return;
	}
	if ( midi_in_sx )
	{
		if ( midi_sx_len < MIDI_SXOUT )
		{
			midi_sx[midi_sx_len++] = b;
		}
		if ( !( b & 0x80 ) )
		{
			return;
		}
		if ( b != 0xF7 && midi_sx_len < MIDI_SXOUT )  // ended by another status: close it off
		{
			midi_sx[midi_sx_len - 1] = 0xF7;
		}
		if ( midi_sys_is_open( 1 ) && midi_sx_len >= 2 )
		{
			midi_sys_send( midi_sx, midi_sx_len );
		}
		midi_in_sx = 0;
		if ( b == 0xF7 )
		{
			return;
		}
	}
	if ( b == 0xF0 )
	{
		midi_in_sx = 1;
		midi_sx[0] = b;
		midi_sx_len = 1;
		midi_status = 0;
		return;
	}
	if ( b & 0x80 )
	{
		midi_status = b;
		midi_msg[0] = b;
		midi_need = midi_data_bytes( b );
		midi_have = 0;
		if ( midi_need == 0 )
		{
			midi_send_short( b, 0, 0, 1 );
			midi_status = b < 0xF0 ? midi_status : 0;
		}
		return;
	}
	if ( !midi_status )
	{
		return;  // data with no status to run on
	}
	midi_msg[1 + midi_have++] = b;
	if ( midi_have == midi_need )
	{
		midi_send_short( midi_msg[0], midi_msg[1], midi_need > 1 ? midi_msg[2] : 0, 1 + midi_need );
		midi_have = 0;
		if ( midi_status >= 0xF0 )  // system common messages do not run on
		{
			midi_status = 0;
		}
	}
}

/**
  * @brief  Take the next byte received from the host input device.
  *
  * @param  b Receives the byte.
  *
  * @retval 1 a byte, 0 none waiting.
  */
int midi_in_byte( uint8_t *b )
{
	int got = 0;

	if ( !midi_sys_is_open( 0 ) )
	{
		return 0;
	}
	midi_sys_poll();
	SDL_AtomicLock( &midi_lock );
	if ( midi_head != midi_tail )
	{
		*b = midi_ring[midi_tail++ % MIDI_RING];
		got = 1;
	}
	SDL_AtomicUnlock( &midi_lock );
	return got;
}

/**
  * @brief  Number of host MIDI input / output devices.
  *
  * @param  out 1 for outputs.
  *
  * @retval The count.
  */
int midi_dev_count( int out )
{
	return midi_sys_count( out );
}

/**
  * @brief  Name of a host MIDI device.
  *
  * @param  out 1 for outputs.
  * @param  i Device index.
  * @param  name Receives the name (MIDI_NAME bytes).
  *
  * @retval 0 found, -1 no such device.
  */
int midi_dev_name( int out, int i, char name[MIDI_NAME] )
{
	name[0] = 0;
	return midi_sys_name( out, i, name );
}
