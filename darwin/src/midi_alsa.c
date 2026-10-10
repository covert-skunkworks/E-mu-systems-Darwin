//  Description:
//
//		midi_alsa.c -- host MIDI on Linux, through the ALSA sequencer (midi_sys.h).
//
//		The sequencer is where Linux MIDI devices and programs meet: a USB
//		interface, a soft synth and "Midi Through" are all ports there. The
//		emulator becomes a client, "E-mu Darwin", with one port per
//		direction, and subscribes them to the chosen ports. Device names are
//		"client: port".
//
//		Three sequencer handles, because a handle must not be used by two
//		threads at once: one for the input thread, one for sending (the
//		emulator thread), and a short-lived one for listing (the settings
//		window). Bytes go to and from sequencer events through ALSA's own
//		MIDI byte coder (snd_midi_event).
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "midi_sys.h"

#include <stdio.h>
#include <string.h>

#include <alsa/asoundlib.h>
#include <SDL2/SDL.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define MA_CODER 65536  // the byte coder's room: the longest SysEx

//------------------------------------------------------------------------------
//---------------------------- Local types and structures ----------------------
//------------------------------------------------------------------------------

typedef struct
{
	int  want;            // the index looked for
	int  k;               // ports counted so far
	int  client, port;    // where it was found (-1: not)
	char name[MIDI_NAME];
} ma_find;

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static snd_seq_t         *ma_in_seq, *ma_out_seq;
static int                ma_in_port = -1, ma_out_port = -1;
static snd_midi_event_t  *ma_enc, *ma_dec;
static SDL_Thread        *ma_thread;
static SDL_atomic_t       ma_run;

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void ma_walk( int out, ma_find *f );
static int ma_reader( void *arg );
static void ma_close_in( void );
static void ma_close_out( void );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Count the ports of one direction, and find the f->want'th.
  *
  * Our own client and the system client (timer, announce) are left out.
  *
  * @param  out 1: ports the Darwin can send to; 0: ports it can listen to.
  * @param  f The search (f->want < 0 only counts).
  *
  * @retval None.
  */
static void ma_walk( int out, ma_find *f )
{
	unsigned need = out ? SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE :
						  SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
	snd_seq_client_info_t *ci;
	snd_seq_port_info_t *pi;
	snd_seq_t *seq;
	int c;

	f->k = 0;
	f->client = f->port = -1;
	if ( snd_seq_open( &seq, "default", SND_SEQ_OPEN_OUTPUT, 0 ) < 0 )
	{
		return;
	}
	snd_seq_client_info_alloca( &ci );
	snd_seq_port_info_alloca( &pi );
	snd_seq_client_info_set_client( ci, -1 );
	while ( snd_seq_query_next_client( seq, ci ) >= 0 )
	{
		c = snd_seq_client_info_get_client( ci );
		if ( c == SND_SEQ_CLIENT_SYSTEM || !strcmp( snd_seq_client_info_get_name( ci ), "E-mu Darwin" ) )
		{
			continue;
		}
		snd_seq_port_info_set_client( pi, c );
		snd_seq_port_info_set_port( pi, -1 );
		while ( snd_seq_query_next_port( seq, pi ) >= 0 )
		{
			if ( ( snd_seq_port_info_get_capability( pi ) & need ) != need ||
				 ( snd_seq_port_info_get_capability( pi ) & SND_SEQ_PORT_CAP_NO_EXPORT ) )
			{
				continue;
			}
			if ( f->k++ == f->want )
			{
				f->client = c;
				f->port = snd_seq_port_info_get_port( pi );
				snprintf( f->name, sizeof f->name, "%s: %s", snd_seq_client_info_get_name( ci ),
						  snd_seq_port_info_get_name( pi ) );
			}
		}
	}
	snd_seq_close( seq );
}

/**
  * @brief  The input thread: sequencer events back to bytes, until told to stop.
  *
  * @param  arg Unused.
  *
  * @retval 0.
  */
static int ma_reader( void *arg )
{
	static unsigned char buf[MA_CODER];
	snd_seq_event_t *ev;
	long n, i;

	(void)arg;
	while ( SDL_AtomicGet( &ma_run ) )
	{
		if ( snd_seq_event_input( ma_in_seq, &ev ) < 0 || !ev )
		{
			SDL_Delay( 1 );  // non-blocking: nothing waiting
			continue;
		}
		n = snd_midi_event_decode( ma_dec, buf, sizeof buf, ev );
		for ( i = 0; i < n; i++ )
		{
			midi_sys_received( buf[i] );
		}
	}
	return 0;
}

/**
  * @brief  Close the input side.
  *
  * @retval None.
  */
static void ma_close_in( void )
{
	if ( ma_thread )
	{
		SDL_AtomicSet( &ma_run, 0 );
		SDL_WaitThread( ma_thread, NULL );
		ma_thread = NULL;
	}
	if ( ma_in_seq )
	{
		snd_seq_close( ma_in_seq );  // drops the port and its subscription
		ma_in_seq = NULL;
		ma_in_port = -1;
	}
	if ( ma_dec )
	{
		snd_midi_event_free( ma_dec );
		ma_dec = NULL;
	}
}

/**
  * @brief  Close the output side.
  *
  * @retval None.
  */
static void ma_close_out( void )
{
	if ( ma_out_seq )
	{
		snd_seq_close( ma_out_seq );
		ma_out_seq = NULL;
		ma_out_port = -1;
	}
	if ( ma_enc )
	{
		snd_midi_event_free( ma_enc );
		ma_enc = NULL;
	}
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Number of host MIDI devices.
  *
  * @param  out 1 for outputs, 0 for inputs.
  *
  * @retval The count.
  */
int midi_sys_count( int out )
{
	ma_find f = { -1, 0, -1, -1, "" };

	ma_walk( out, &f );
	return f.k;
}

/**
  * @brief  A device's name.
  *
  * @param  out 1 for outputs.
  * @param  i Device index.
  * @param  name Receives the name.
  *
  * @retval 0 found, -1 no such device.
  */
int midi_sys_name( int out, int i, char name[MIDI_NAME] )
{
	ma_find f = { i, 0, -1, -1, "" };

	ma_walk( out, &f );
	if ( f.client < 0 )
	{
		return -1;
	}
	snprintf( name, MIDI_NAME, "%s", f.name );
	return 0;
}

/**
  * @brief  Open a device (the one of that direction already open is closed first).
  *
  * @param  out 1 for an output.
  * @param  i Device index.
  *
  * @retval 0 open, -1 failed.
  */
int midi_sys_open( int out, int i )
{
	ma_find f = { i, 0, -1, -1, "" };
	snd_seq_t **seq = out ? &ma_out_seq : &ma_in_seq;
	int *port = out ? &ma_out_port : &ma_in_port;

	if ( out )
	{
		ma_close_out();
	}
	else
	{
		ma_close_in();
	}
	ma_walk( out, &f );
	if ( f.client < 0 || snd_seq_open( seq, "default", out ? SND_SEQ_OPEN_OUTPUT : SND_SEQ_OPEN_INPUT,
									  out ? 0 : SND_SEQ_NONBLOCK ) < 0 )
	{
		*seq = NULL;
		return -1;
	}
	snd_seq_set_client_name( *seq, "E-mu Darwin" );
	*port = snd_seq_create_simple_port( *seq, out ? "MIDI out" : "MIDI in",
										out ? SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ :
											  SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
										SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION );
	if ( *port < 0 || ( out ? snd_seq_connect_to( *seq, *port, f.client, f.port ) :
							  snd_seq_connect_from( *seq, *port, f.client, f.port ) ) < 0 ||
		 snd_midi_event_new( MA_CODER, out ? &ma_enc : &ma_dec ) < 0 )
	{
		if ( out )
		{
			ma_close_out();
		}
		else
		{
			ma_close_in();
		}
		return -1;
	}
	if ( !out )
	{
		snd_midi_event_no_status( ma_dec, 1 );  // every message with its status: the DUART wants plain bytes
		SDL_AtomicSet( &ma_run, 1 );
		ma_thread = SDL_CreateThread( ma_reader, "midi in", NULL );
		if ( !ma_thread )
		{
			ma_close_in();
			return -1;
		}
	}
	return 0;
}

/**
  * @brief  Close both directions.
  *
  * @retval None.
  */
void midi_sys_close( void )
{
	ma_close_in();
	ma_close_out();
}

/**
  * @brief  Is a direction open?
  *
  * @param  out 1 for the output.
  *
  * @retval 1 open.
  */
int midi_sys_is_open( int out )
{
	return out ? ma_out_seq != NULL : ma_in_seq != NULL;
}

/**
  * @brief  Send one whole message: a short message (1-3 bytes) or SysEx (F0 ... F7).
  *
  * @param  d The bytes.
  * @param  n How many.
  *
  * @retval None.
  */
void midi_sys_send( const uint8_t *d, int n )
{
	snd_seq_event_t ev;
	long used;

	if ( !ma_out_seq || n < 1 )
	{
		return;
	}
	snd_midi_event_reset_encode( ma_enc );
	while ( n > 0 )
	{
		snd_seq_ev_clear( &ev );
		used = snd_midi_event_encode( ma_enc, d, n, &ev );
		if ( used <= 0 )
		{
			return;
		}
		d += used;
		n -= (int)used;
		if ( ev.type != SND_SEQ_EVENT_NONE )
		{
			snd_seq_ev_set_source( &ev, ma_out_port );
			snd_seq_ev_set_subs( &ev );
			snd_seq_ev_set_direct( &ev );
			snd_seq_event_output_direct( ma_out_seq, &ev );
		}
	}
}

/**
  * @brief  The emulator thread is about to take received bytes: nothing to do here.
  *
  * @retval None.
  */
void midi_sys_poll( void )
{
}
