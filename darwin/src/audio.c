//  Description:
//
//		audio.c -- the Darwin's converters on the host's sound devices.
//
//		The emulator thread produces one frame per emulated sample period
//		(au_frame, called by the ASIC); SDL's device threads play and record
//		in real time. Between them are two single-producer single-consumer
//		rings, so neither side waits for the other:
//
//		  emulator --out ring--> playback callback  (frames of 12 outputs)
//		  emulator <--in ring--- recording callback (frames of 6 inputs)
//
//		The emulator is paced to the wall clock (main.c), so the rings stay
//		near their working fill; if they drift, the playback side waits for
//		AU_PREFILL frames after running dry and the producer drops frames
//		rather than block. The routing (settings in1-10, out1-12) is read in
//		the callbacks, so changing it in the settings window applies at
//		once; changing a device needs au_close / au_open (power cycle).
//
//		For runs without a device, -wav writes the mix outputs to a file
//		and -wavin plays a file into analog inputs 1 and 2.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "audio.h"
#include "darwin.h"
#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define AU_RING    8192  // frames per ring (a power of two)
#define AU_PREFILL 2048  // frames buffered before playback (re)starts: ~46 ms
#define AU_BLOCK   512   // device buffer, frames

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static int16_t           au_out_ring[AU_RING][AU_OUTS];
static int16_t           au_in_ring[AU_RING][AU_INS];
static SDL_atomic_t      au_out_w, au_out_r, au_in_w, au_in_r;
static SDL_AudioDeviceID au_play, au_rec;
static int               au_play_ch, au_rec_ch;
static int               au_waiting = 1;    // playback: building up AU_PREFILL first
static int16_t           au_in_last[AU_INS];
static FILE             *au_wout, *au_win;
static uint32_t          au_wout_frames;
static int               au_win_ch;
static volatile float    au_phones = 1.0f;  // headphone gain (GUI thread writes, emulator reads)

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void SDLCALL au_play_cb( void *ud, Uint8 *stream, int len );
static void SDLCALL au_rec_cb( void *ud, Uint8 *stream, int len );
static SDL_AudioDeviceID au_open_dev( const char *name, int capture, int channels, int *got );
static void au_wr16( FILE *f, unsigned v );
static void au_wr32( FILE *f, uint32_t v );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Playback callback: route queued frames to the device's channels.
  *
  * @param  ud Unused.
  * @param  stream The device buffer, S16 interleaved.
  * @param  len Its size in bytes.
  *
  * @retval None.
  */
static void SDLCALL au_play_cb( void *ud, Uint8 *stream, int len )
{
	int16_t *o = (int16_t *)stream;
	int n = len / ( 2 * au_play_ch ), f, k, c, s;
	unsigned r = (unsigned)SDL_AtomicGet( &au_out_r ), w = (unsigned)SDL_AtomicGet( &au_out_w );

	(void)ud;
	memset( stream, 0, (size_t)len );
	if ( au_waiting )
	{
		if ( w - r < AU_PREFILL )
		{
			return;
		}
		au_waiting = 0;
	}
	for ( f = 0; f < n; f++, r++ )
	{
		if ( r == w )
		{
			au_waiting = 1;  // ran dry: build up again before playing on
			break;
		}
		for ( k = 0; k < AU_OUTS; k++ )
		{
			c = set_cfg.out_ch[k];
			if ( c >= 1 && c <= au_play_ch )
			{
				s = o[f * au_play_ch + c - 1] + au_out_ring[r & ( AU_RING - 1 )][k];
				o[f * au_play_ch + c - 1] = (int16_t)( s > 32767 ? 32767 : s < -32768 ? -32768 : s );
			}
		}
	}
	SDL_AtomicSet( &au_out_r, (int)r );
}

/**
  * @brief  Recording callback: take the routed channels into the in ring.
  *
  * @param  ud Unused.
  * @param  stream The device buffer, S16 interleaved.
  * @param  len Its size in bytes.
  *
  * @retval None.
  */
static void SDLCALL au_rec_cb( void *ud, Uint8 *stream, int len )
{
	const int16_t *s = (const int16_t *)stream;
	int n = len / ( 2 * au_rec_ch ), f, k, c;
	unsigned w = (unsigned)SDL_AtomicGet( &au_in_w ), r = (unsigned)SDL_AtomicGet( &au_in_r );

	(void)ud;
	for ( f = 0; f < n && w - r < AU_RING; f++, w++ )
	{
		for ( k = 0; k < AU_INS; k++ )
		{
			c = set_cfg.in_ch[k];
			au_in_ring[w & ( AU_RING - 1 )][k] = c >= 1 && c <= au_rec_ch ? s[f * au_rec_ch + c - 1] : 0;
		}
	}
	SDL_AtomicSet( &au_in_w, (int)w );
}

/**
  * @brief  Open a device by name, asking for enough channels for the routing.
  *
  * @param  name Device name, "" for the default.
  * @param  capture 1 for a recording device.
  * @param  channels Channels wanted.
  * @param  got Receives the channels granted.
  *
  * @retval The device, 0 if it would not open.
  */
static SDL_AudioDeviceID au_open_dev( const char *name, int capture, int channels, int *got )
{
	SDL_AudioSpec want, have;
	SDL_AudioDeviceID d;

	memset( &want, 0, sizeof want );
	want.freq = AU_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = (Uint8)( channels > 8 ? 8 : channels < 1 ? 1 : channels );
	want.samples = AU_BLOCK;
	want.callback = capture ? au_rec_cb : au_play_cb;
	d = SDL_OpenAudioDevice( name[0] ? name : NULL, capture, &want, &have, SDL_AUDIO_ALLOW_CHANNELS_CHANGE );
	if ( d )
	{
		*got = have.channels;
		dw_log( "audio: %s %s, %d channels\n", capture ? "recording from" : "playing on",
				name[0] ? name : "the default device", have.channels );
	}
	else
	{
		dw_log( "audio: cannot open %s device %s: %s\n", capture ? "recording" : "playback",
				name[0] ? name : "(default)", SDL_GetError() );
	}
	return d;
}

/**
  * @brief  Write a little-endian 16-bit value.
  *
  * @param  f The file.
  * @param  v The value.
  *
  * @retval None.
  */
static void au_wr16( FILE *f, unsigned v )
{
	fputc( (int)( v & 0xFF ), f );
	fputc( (int)( ( v >> 8 ) & 0xFF ), f );
}

/**
  * @brief  Write a little-endian 32-bit value.
  *
  * @param  f The file.
  * @param  v The value.
  *
  * @retval None.
  */
static void au_wr32( FILE *f, uint32_t v )
{
	au_wr16( f, v & 0xFFFF );
	au_wr16( f, v >> 16 );
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Open the host devices named in the settings (playback, recording).
  *
  * @retval 0 playing, -1 no playback device (the machine runs silent).
  */
int au_open( void )
{
	int k, need = 2;

	if ( !SDL_WasInit( SDL_INIT_AUDIO ) && SDL_InitSubSystem( SDL_INIT_AUDIO ) != 0 )
	{
		dw_log( "audio: %s\n", SDL_GetError() );
		return -1;
	}
	SDL_AtomicSet( &au_out_w, 0 );
	SDL_AtomicSet( &au_out_r, 0 );
	SDL_AtomicSet( &au_in_w, 0 );
	SDL_AtomicSet( &au_in_r, 0 );
	au_waiting = 1;
	for ( k = 0; k < AU_OUTS; k++ )
	{
		need = set_cfg.out_ch[k] > need ? set_cfg.out_ch[k] : need;
	}
	au_play = au_open_dev( set_cfg.audio_out, 0, need, &au_play_ch );
	if ( strcmp( set_cfg.audio_in, "-" ) )
	{
		for ( need = 1, k = 0; k < AU_INS; k++ )
		{
			need = set_cfg.in_ch[k] > need ? set_cfg.in_ch[k] : need;
		}
		au_rec = au_open_dev( set_cfg.audio_in, 1, need, &au_rec_ch );
	}
	if ( au_play )
	{
		SDL_PauseAudioDevice( au_play, 0 );
	}
	if ( au_rec )
	{
		SDL_PauseAudioDevice( au_rec, 0 );
	}
	return au_play ? 0 : -1;
}

/**
  * @brief  Close the host devices.
  *
  * @retval None.
  */
void au_close( void )
{
	if ( au_play )
	{
		SDL_CloseAudioDevice( au_play );
		au_play = 0;
	}
	if ( au_rec )
	{
		SDL_CloseAudioDevice( au_rec );
		au_rec = 0;
	}
}

/**
  * @brief  One sample period: the outputs go to the host, the inputs come back.
  *
  * @param  out The twelve outputs.
  * @param  in Receives the six inputs (silence when there is no input).
  *
  * @retval None.
  */
void au_frame( const int16_t out[AU_OUTS], int16_t in[AU_INS] )
{
	uint8_t b[16];
	unsigned w, r;

	if ( au_play )
	{
		w = (unsigned)SDL_AtomicGet( &au_out_w );
		r = (unsigned)SDL_AtomicGet( &au_out_r );
		if ( w - r < AU_RING )  // full: the device has stalled, drop rather than wait
		{
			memcpy( au_out_ring[w & ( AU_RING - 1 )], out, sizeof au_out_ring[0] );
			SDL_AtomicSet( &au_out_w, (int)( w + 1 ) );
		}
	}
	if ( au_wout )
	{
		au_wr16( au_wout, (uint16_t)out[8] );
		au_wr16( au_wout, (uint16_t)out[9] );
		au_wout_frames++;
	}
	memset( in, 0, sizeof( int16_t ) * AU_INS );
	if ( au_win )
	{
		if ( fread( b, 2, (size_t)au_win_ch, au_win ) == (size_t)au_win_ch )
		{
			in[0] = (int16_t)( b[0] | b[1] << 8 );
			in[1] = au_win_ch > 1 ? (int16_t)( b[2] | b[3] << 8 ) : in[0];
		}
		return;
	}
	if ( au_rec )
	{
		w = (unsigned)SDL_AtomicGet( &au_in_w );
		r = (unsigned)SDL_AtomicGet( &au_in_r );
		if ( w - r > AU_RING / 2 )  // the device is ahead: skip to stay near real time
		{
			r = w - AU_PREFILL;
		}
		if ( r != w )
		{
			memcpy( au_in_last, au_in_ring[r & ( AU_RING - 1 )], sizeof au_in_last );
			SDL_AtomicSet( &au_in_r, (int)( r + 1 ) );
		}
		memcpy( in, au_in_last, sizeof au_in_last );  // dry for a moment: hold the last frame
	}
}

/**
  * @brief  Set the headphone volume knob (an analog pot on the real unit, after the mix).
  *
  * @param  pos Knob position 0-1: 0 off, then -48 dB to 0 dB along a log taper.
  *
  * @retval None.
  */
void au_set_phones( float pos )
{
	pos = pos < 0 ? 0 : pos > 1 ? 1 : pos;
	au_phones = pos <= 0.005f ? 0.0f : powf( 10.0f, ( pos - 1.0f ) * 48.0f / 20.0f );
}

/**
  * @brief  The headphone gain the knob gives.
  *
  * @retval Linear gain 0-1.
  */
float au_phones_gain( void )
{
	return au_phones;
}

/**
  * @brief  Also write the mix outputs to a WAV file (-wav), for runs without a device.
  *
  * @param  path The file.
  *
  * @retval 0 open, -1 failed.
  */
int au_wav_out( const char *path )
{
	au_wout = fopen( path, "wb" );
	if ( !au_wout )
	{
		return -1;
	}
	// a 16-bit stereo header; the sizes are filled in by au_wav_close
	fwrite( "RIFF", 1, 4, au_wout );
	au_wr32( au_wout, 0 );
	fwrite( "WAVEfmt ", 1, 8, au_wout );
	au_wr32( au_wout, 16 );
	au_wr16( au_wout, 1 );
	au_wr16( au_wout, 2 );
	au_wr32( au_wout, AU_RATE );
	au_wr32( au_wout, AU_RATE * 4 );
	au_wr16( au_wout, 4 );
	au_wr16( au_wout, 16 );
	fwrite( "data", 1, 4, au_wout );
	au_wr32( au_wout, 0 );
	au_wout_frames = 0;
	return 0;
}

/**
  * @brief  Feed the analog inputs from a WAV file instead of the device (-wavin).
  *
  * 16-bit PCM; channel 1 feeds input 1, channel 2 input 2.
  *
  * @param  path The file.
  *
  * @retval 0 open, -1 failed or not 16-bit PCM.
  */
int au_wav_in( const char *path )
{
	uint8_t h[24];
	uint32_t len;

	au_win = fopen( path, "rb" );
	if ( !au_win || fread( h, 1, 12, au_win ) != 12 || memcmp( h, "RIFF", 4 ) || memcmp( h + 8, "WAVE", 4 ) )
	{
		goto bad;
	}
	au_win_ch = 0;
	while ( fread( h, 1, 8, au_win ) == 8 )  // walk the chunks to "data"
	{
		len = (uint32_t)( h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24 );
		if ( !memcmp( h, "fmt ", 4 ) )
		{
			if ( len < 16 || fread( h + 8, 1, 16, au_win ) != 16 || ( h[8] | h[9] << 8 ) != 1 || h[22] != 16 )
			{
				goto bad;
			}
			au_win_ch = h[10] | h[11] << 8;
			fseek( au_win, (long)( len - 16 + ( len & 1 ) ), SEEK_CUR );
		}
		else if ( !memcmp( h, "data", 4 ) )
		{
			if ( au_win_ch < 1 || au_win_ch > 8 )
			{
				goto bad;
			}
			return 0;
		}
		else
		{
			fseek( au_win, (long)( len + ( len & 1 ) ), SEEK_CUR );
		}
	}
bad:
	if ( au_win )
	{
		fclose( au_win );
		au_win = NULL;
	}
	return -1;
}

/**
  * @brief  Finish the WAV files (sizes in their headers).
  *
  * @retval None.
  */
void au_wav_close( void )
{
	if ( au_wout )
	{
		fseek( au_wout, 4, SEEK_SET );
		au_wr32( au_wout, 36 + au_wout_frames * 4 );
		fseek( au_wout, 40, SEEK_SET );
		au_wr32( au_wout, au_wout_frames * 4 );
		fclose( au_wout );
		au_wout = NULL;
	}
	if ( au_win )
	{
		fclose( au_win );
		au_win = NULL;
	}
}
