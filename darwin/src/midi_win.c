//  Description:
//
//		midi_win.c -- host MIDI on Windows, through winmm (midi_sys.h).
//
//		winmm calls back on its own thread with whole messages: short ones
//		packed in a DWORD, SysEx in buffers. The SysEx buffers are handed
//		back to winmm from the emulator thread (midi_sys_poll), since winmm
//		should not be called from its own callback.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "midi_sys.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <mmsystem.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define MW_SXBUF  1024   // a SysEx input buffer
#define MW_SXBUFS 4

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static HMIDIIN       mw_hin;
static HMIDIOUT      mw_hout;
static MIDIHDR       mw_sxhdr[MW_SXBUFS];
static char          mw_sxin[MW_SXBUFS][MW_SXBUF];
static volatile LONG mw_sx_done[MW_SXBUFS];  // a buffer came back and wants re-adding

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void CALLBACK mw_in_cb( HMIDIIN h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2 );
static void mw_close_in( void );
static void mw_close_out( void );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  winmm input callback (its own thread): pass the bytes on.
  *
  * @param  h The device.
  * @param  msg MIM_DATA, MIM_LONGDATA, ...
  * @param  inst Unused.
  * @param  p1 The packed message, or the MIDIHDR.
  * @param  p2 Time stamp, unused.
  *
  * @retval None.
  */
static void CALLBACK mw_in_cb( HMIDIIN h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2 )
{
	MIDIHDR *hdr;
	uint8_t st;
	DWORD i;
	int n, k;

	(void)h;
	(void)inst;
	(void)p2;
	if ( msg == MIM_DATA )
	{
		st = (uint8_t)p1;
		if ( !( st & 0x80 ) )
		{
			return;
		}
		n = st < 0xF0 ? ( ( st & 0xE0 ) == 0xC0 ? 1 : 2 ) : st == 0xF1 || st == 0xF3 ? 1 : st == 0xF2 ? 2 : 0;
		midi_sys_received( st );
		for ( k = 0; k < n; k++ )
		{
			midi_sys_received( (uint8_t)( p1 >> ( 8 * ( k + 1 ) ) ) );
		}
	}
	else if ( msg == MIM_LONGDATA )
	{
		hdr = (MIDIHDR *)p1;
		for ( i = 0; i < hdr->dwBytesRecorded; i++ )
		{
			midi_sys_received( (uint8_t)hdr->lpData[i] );
		}
		InterlockedExchange( &mw_sx_done[hdr->dwUser & ( MW_SXBUFS - 1 )], 1 );
	}
}

/**
  * @brief  Close the input device.
  *
  * @retval None.
  */
static void mw_close_in( void )
{
	int i;

	if ( !mw_hin )
	{
		return;
	}
	midiInStop( mw_hin );
	midiInReset( mw_hin );  // returns the SysEx buffers
	for ( i = 0; i < MW_SXBUFS; i++ )
	{
		midiInUnprepareHeader( mw_hin, &mw_sxhdr[i], sizeof mw_sxhdr[i] );
	}
	midiInClose( mw_hin );
	mw_hin = NULL;
}

/**
  * @brief  Close the output device.
  *
  * @retval None.
  */
static void mw_close_out( void )
{
	if ( mw_hout )
	{
		midiOutReset( mw_hout );
		midiOutClose( mw_hout );
		mw_hout = NULL;
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
	return (int)( out ? midiOutGetNumDevs() : midiInGetNumDevs() );
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
	MIDIOUTCAPSA oc;
	MIDIINCAPSA ic;

	if ( out )
	{
		if ( midiOutGetDevCapsA( (UINT_PTR)i, &oc, sizeof oc ) != MMSYSERR_NOERROR )
		{
			return -1;
		}
		snprintf( name, MIDI_NAME, "%s", oc.szPname );
	}
	else
	{
		if ( midiInGetDevCapsA( (UINT_PTR)i, &ic, sizeof ic ) != MMSYSERR_NOERROR )
		{
			return -1;
		}
		snprintf( name, MIDI_NAME, "%s", ic.szPname );
	}
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
	int k;

	if ( out )
	{
		mw_close_out();
		if ( midiOutOpen( &mw_hout, (UINT)i, 0, 0, CALLBACK_NULL ) != MMSYSERR_NOERROR )
		{
			mw_hout = NULL;
			return -1;
		}
		return 0;
	}
	mw_close_in();
	if ( midiInOpen( &mw_hin, (UINT)i, (DWORD_PTR)mw_in_cb, 0, CALLBACK_FUNCTION ) != MMSYSERR_NOERROR )
	{
		mw_hin = NULL;
		return -1;
	}
	for ( k = 0; k < MW_SXBUFS; k++ )
	{
		memset( &mw_sxhdr[k], 0, sizeof mw_sxhdr[k] );
		mw_sxhdr[k].lpData = mw_sxin[k];
		mw_sxhdr[k].dwBufferLength = MW_SXBUF;
		mw_sxhdr[k].dwUser = (DWORD_PTR)k;
		mw_sx_done[k] = 0;
		midiInPrepareHeader( mw_hin, &mw_sxhdr[k], sizeof mw_sxhdr[k] );
		midiInAddBuffer( mw_hin, &mw_sxhdr[k], sizeof mw_sxhdr[k] );
	}
	midiInStart( mw_hin );
	return 0;
}

/**
  * @brief  Close both directions.
  *
  * @retval None.
  */
void midi_sys_close( void )
{
	mw_close_in();
	mw_close_out();
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
	return out ? mw_hout != NULL : mw_hin != NULL;
}

/**
  * @brief  Send one whole message: a short message (1-3 bytes) or SysEx (F0 ... F7).
  *
  * SysEx waits for the driver to finish with it (at most half a second).
  *
  * @param  d The bytes.
  * @param  n How many.
  *
  * @retval None.
  */
void midi_sys_send( const uint8_t *d, int n )
{
	MIDIHDR h;
	int i;

	if ( !mw_hout || n < 1 )
	{
		return;
	}
	if ( d[0] != 0xF0 )
	{
		midiOutShortMsg( mw_hout, (DWORD)d[0] | ( n > 1 ? (DWORD)d[1] << 8 : 0 ) | ( n > 2 ? (DWORD)d[2] << 16 : 0 ) );
		return;
	}
	memset( &h, 0, sizeof h );
	h.lpData = (LPSTR)d;
	h.dwBufferLength = (DWORD)n;
	if ( midiOutPrepareHeader( mw_hout, &h, sizeof h ) != MMSYSERR_NOERROR )
	{
		return;
	}
	if ( midiOutLongMsg( mw_hout, &h, sizeof h ) == MMSYSERR_NOERROR )
	{
		for ( i = 0; i < 500 && !( h.dwFlags & MHDR_DONE ); i++ )
		{
			Sleep( 1 );
		}
	}
	midiOutUnprepareHeader( mw_hout, &h, sizeof h );
}

/**
  * @brief  The emulator thread is about to take received bytes: SysEx buffers winmm has
  *         given back go round again.
  *
  * @retval None.
  */
void midi_sys_poll( void )
{
	int i;

	for ( i = 0; mw_hin && i < MW_SXBUFS; i++ )
	{
		if ( InterlockedExchange( &mw_sx_done[i], 0 ) )
		{
			midiInAddBuffer( mw_hin, &mw_sxhdr[i], sizeof mw_sxhdr[i] );
		}
	}
}
