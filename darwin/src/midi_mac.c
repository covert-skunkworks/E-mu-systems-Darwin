//  Description:
//
//		midi_mac.c -- host MIDI on macOS, through CoreMIDI (midi_sys.h).
//
//		Inputs are CoreMIDI's sources, outputs its destinations, named by
//		their display names. The emulator is a CoreMIDI client, "E-mu
//		Darwin", with an input port connected to the chosen source and an
//		output port that sends to the chosen destination. CoreMIDI calls the
//		read procedure on its own thread with packets of plain MIDI bytes,
//		SysEx included.
//
//		MIDIInputPortCreate and its MIDIPacketList have newer replacements
//		(MIDI 2.0 event lists, macOS 11) but still work everywhere; the
//		Makefile quietens their deprecation warnings.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "midi_sys.h"

#include <stdio.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define MM_PACKETS ( 65536 + 1024 )  // room for a packet list holding the longest SysEx

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static MIDIClientRef   mm_client;
static MIDIPortRef     mm_in_port, mm_out_port;
static MIDIEndpointRef mm_src, mm_dst;
static int             mm_in_open, mm_out_open;
static Byte            mm_list[MM_PACKETS];  // the packet list being sent

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static int mm_client_ready( void );
static void mm_read( const MIDIPacketList *pl, void *port_ref, void *src_ref );
static MIDIEndpointRef mm_endpoint( int out, int i );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Create the CoreMIDI client and its two ports, once.
  *
  * @retval 1 ready, 0 failed.
  */
static int mm_client_ready( void )
{
	if ( mm_client )
	{
		return 1;
	}
	if ( MIDIClientCreate( CFSTR( "E-mu Darwin" ), NULL, NULL, &mm_client ) != noErr )
	{
		mm_client = 0;
		return 0;
	}
	if ( MIDIInputPortCreate( mm_client, CFSTR( "MIDI in" ), mm_read, NULL, &mm_in_port ) != noErr ||
		 MIDIOutputPortCreate( mm_client, CFSTR( "MIDI out" ), &mm_out_port ) != noErr )
	{
		MIDIClientDispose( mm_client );
		mm_client = 0;
		return 0;
	}
	return 1;
}

/**
  * @brief  CoreMIDI's read procedure (its own thread): pass the bytes on.
  *
  * @param  pl The packets.
  * @param  port_ref Unused.
  * @param  src_ref Unused.
  *
  * @retval None.
  */
static void mm_read( const MIDIPacketList *pl, void *port_ref, void *src_ref )
{
	const MIDIPacket *p = &pl->packet[0];
	UInt32 i;
	UInt16 j;

	(void)port_ref;
	(void)src_ref;
	if ( !mm_in_open )
	{
		return;
	}
	for ( i = 0; i < pl->numPackets; i++ )
	{
		for ( j = 0; j < p->length; j++ )
		{
			midi_sys_received( p->data[j] );
		}
		p = MIDIPacketNext( p );
	}
}

/**
  * @brief  A source or destination by index.
  *
  * @param  out 1 for a destination.
  * @param  i Index.
  *
  * @retval The endpoint, 0 if there is none.
  */
static MIDIEndpointRef mm_endpoint( int out, int i )
{
	if ( i < 0 || i >= midi_sys_count( out ) )
	{
		return 0;
	}
	return out ? MIDIGetDestination( (ItemCount)i ) : MIDIGetSource( (ItemCount)i );
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
	return (int)( out ? MIDIGetNumberOfDestinations() : MIDIGetNumberOfSources() );
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
	MIDIEndpointRef e = mm_endpoint( out, i );
	CFStringRef s = NULL;
	int ok;

	if ( !e || MIDIObjectGetStringProperty( e, kMIDIPropertyDisplayName, &s ) != noErr || !s )
	{
		return -1;
	}
	ok = CFStringGetCString( s, name, MIDI_NAME, kCFStringEncodingUTF8 );
	CFRelease( s );
	return ok ? 0 : -1;
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
	MIDIEndpointRef e = mm_endpoint( out, i );

	if ( !e || !mm_client_ready() )
	{
		return -1;
	}
	if ( out )
	{
		mm_dst = e;
		mm_out_open = 1;
		return 0;
	}
	if ( mm_in_open )
	{
		MIDIPortDisconnectSource( mm_in_port, mm_src );
		mm_in_open = 0;
	}
	if ( MIDIPortConnectSource( mm_in_port, e, NULL ) != noErr )
	{
		return -1;
	}
	mm_src = e;
	mm_in_open = 1;
	return 0;
}

/**
  * @brief  Close both directions.
  *
  * @retval None.
  */
void midi_sys_close( void )
{
	if ( mm_in_open )
	{
		mm_in_open = 0;
		MIDIPortDisconnectSource( mm_in_port, mm_src );
	}
	mm_out_open = 0;
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
	return out ? mm_out_open : mm_in_open;
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
	MIDIPacketList *pl = (MIDIPacketList *)mm_list;
	MIDIPacket *p;

	if ( !mm_out_open || n < 1 || n > 65535 )
	{
		return;
	}
	p = MIDIPacketListInit( pl );
	p = MIDIPacketListAdd( pl, sizeof mm_list, p, 0, (ByteCount)n, d );
	if ( p )
	{
		MIDISend( mm_out_port, mm_dst, pl );
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
