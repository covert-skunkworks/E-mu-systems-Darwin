//  Description:
//
//		settings.c -- Emu_Darwin.ini next to the program.
//
//		Unknown keys are ignored, so a newer file still loads.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

// a stereo host by default: the mix on channels 1 / 2, analog inputs 1 / 2 from channels 1 / 2
set_config  set_cfg = { .debug_duart = 1, .in_ch = { 1, 2 }, .out_ch = { [8] = 1, [9] = 2 }, .phones_volume = 100,
						.keyboard = 1 };
static char set_file[SET_PATH];

const char *const set_in_names[SET_INS] = { "Analog in 1", "Analog in 2", "Analog in 3", "Analog in 4",
	"Analog in 5", "Analog in 6", "Analog in 7", "Analog in 8", "S/PDIF in L", "S/PDIF in R" };
const char *const set_out_names[SET_OUTS] = { "Track out 1", "Track out 2", "Track out 3", "Track out 4",
	"Track out 5", "Track out 6", "Track out 7", "Track out 8", "Phones L (mix)", "Phones R (mix)", "S/PDIF out L (mix)", "S/PDIF out R (mix)" };

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void set_trim( char *s );
static int set_channel( const char *v );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Strip leading and trailing white space in place.
  *
  * @param  s The string.
  *
  * @retval None.
  */
static void set_trim( char *s )
{
	char *p = s;
	size_t n;

	while ( *p == ' ' || *p == '\t' )
	{
		p++;
	}
	memmove( s, p, strlen( p ) + 1 );
	n = strlen( s );
	while ( n && ( s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n' ) )
	{
		s[--n] = 0;
	}
}

/**
  * @brief  A host channel number from the file.
  *
  * @param  v The value.
  *
  * @retval 1..SET_CH, or 0 (off) for anything else.
  */
static int set_channel( const char *v )
{
	int n = atoi( v );

	return n < 1 || n > SET_CH ? 0 : n;
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Read the settings file (missing file: defaults).
  *
  * @param  path The file.
  *
  * @retval 0 read, 1 no file.
  */
int set_load( const char *path )
{
	char line[SET_PATH + 32], *eq;
	FILE *f;
	int bus, id, n;

	snprintf( set_file, sizeof set_file, "%s", path );
	f = fopen( path, "r" );
	if ( !f )
	{
		return 1;
	}
	while ( fgets( line, sizeof line, f ) )
	{
		set_trim( line );
		if ( !line[0] || line[0] == '#' || line[0] == ';' || !( eq = strchr( line, '=' ) ) )
		{
			continue;
		}
		*eq = 0;
		set_trim( line );
		set_trim( eq + 1 );
		bus = !strncmp( line, "audio", 5 ) && line[5] >= '0' && line[5] <= '9' ? 1 :
			  !strncmp( line, "host", 4 ) ? 0 : -1;
		if ( bus >= 0 )
		{
			id = line[bus ? 5 : 4] - '0';
			if ( id >= 0 && id < 8 && line[bus ? 6 : 5] == 0 )
			{
				snprintf( set_cfg.disk[bus][id], SET_PATH, "%s", eq + 1 );
			}
		}
		else if ( !strcmp( line, "debug_duart" ) )
		{
			set_cfg.debug_duart = eq[1] != '0';
		}
		else if ( !strcmp( line, "midi_in" ) )
		{
			snprintf( set_cfg.midi_in, SET_DEV, "%s", eq + 1 );
		}
		else if ( !strcmp( line, "midi_out" ) )
		{
			snprintf( set_cfg.midi_out, SET_DEV, "%s", eq + 1 );
		}
		else if ( !strcmp( line, "phones_volume" ) )
		{
			n = atoi( eq + 1 );
			set_cfg.phones_volume = n < 0 ? 0 : n > 100 ? 100 : n;
		}
		else if ( !strcmp( line, "ui_half" ) )
		{
			set_cfg.ui_half = eq[1] == '1';
		}
		else if ( !strcmp( line, "fast_panel" ) )
		{
			set_cfg.fast_panel = eq[1] == '1';
		}
		else if ( !strncmp( line, "key.", 4 ) || !strncmp( line, "key2.", 5 ) )
		{
			set_key_put( line, eq + 1 );
		}
		else if ( !strcmp( line, "firmware" ) )
		{
			snprintf( set_cfg.firmware, SET_DEV, "%s", eq + 1 );
		}
		else if ( !strcmp( line, "keyboard" ) )
		{
			set_cfg.keyboard = eq[1] != '0';
		}
		else if ( !strcmp( line, "audio_out" ) )
		{
			snprintf( set_cfg.audio_out, SET_DEV, "%s", eq + 1 );
		}
		else if ( !strcmp( line, "audio_in" ) )
		{
			snprintf( set_cfg.audio_in, SET_DEV, "%s", eq + 1 );
		}
		else if ( sscanf( line, "in%d", &n ) == 1 && n >= 1 && n <= SET_INS )
		{
			set_cfg.in_ch[n - 1] = set_channel( eq + 1 );
		}
		else if ( sscanf( line, "out%d", &n ) == 1 && n >= 1 && n <= SET_OUTS )
		{
			set_cfg.out_ch[n - 1] = set_channel( eq + 1 );
		}
	}
	fclose( f );
	return 0;
}

/**
  * @brief  Write the settings file.
  *
  * @retval 0 written, -1 failed.
  */
int set_save( void )
{
	FILE *f = set_file[0] ? fopen( set_file, "w" ) : NULL;
	int bus, id;

	if ( !f )
	{
		return -1;
	}
	fprintf( f, "# E-mu Darwin emulator settings (the Darwin's own settings: darwin_nvram_<version>.bin, one per firmware)\n" );
	fprintf( f, "# audioN / hostN = disk image at SCSI ID N on the audio / host bus\n" );
	for ( bus = 1; bus >= 0; bus-- )
	{
		for ( id = 0; id < 8; id++ )
		{
			if ( set_cfg.disk[bus][id][0] )
			{
				fprintf( f, "%s%d=%s\n", bus ? "audio" : "host", id, set_cfg.disk[bus][id] );
			}
		}
	}
	fprintf( f, "debug_duart=%d\n", set_cfg.debug_duart );
	fprintf( f, "fast_panel=%d\n", set_cfg.fast_panel );
	fprintf( f, "ui_half=%d\n", set_cfg.ui_half );
	fprintf( f, "keyboard=%d\n", set_cfg.keyboard );
	fprintf( f, "# firmware zip to run (empty = the newest found); each has its own darwin_nvram_<version>.bin\n" );
	fprintf( f, "firmware=%s\n", set_cfg.firmware );
	fprintf( f, "phones_volume=%d\n", set_cfg.phones_volume );
	fprintf( f, "# host MIDI devices on the Darwin's MIDI in / out (empty = none)\n" );
	fprintf( f, "midi_in=%s\nmidi_out=%s\n", set_cfg.midi_in, set_cfg.midi_out );
	fprintf( f, "# host audio devices (empty = the Windows default; audio_in=- for none), then the host\n" );
	fprintf( f, "# channel per Darwin input (in1-8 analog, in9-10 S/PDIF L/R) and output (out1-8 tracks,\n" );
	fprintf( f, "# out9-10 phones L/R, out11-12 S/PDIF out L/R, both the mix); 0 = off\n" );
	fprintf( f, "audio_out=%s\naudio_in=%s\n", set_cfg.audio_out, set_cfg.audio_in );
	for ( id = 0; id < SET_INS; id++ )
	{
		fprintf( f, "in%d=%d\n", id + 1, set_cfg.in_ch[id] );
	}
	for ( id = 0; id < SET_OUTS; id++ )
	{
		fprintf( f, "out%d=%d\n", id + 1, set_cfg.out_ch[id] );
	}
	fprintf( f, "# changed keyboard keys of the panel controls (key2 = the alternate key; empty = none)\n" );
	for ( id = 0; id < set_cfg.nkeys; id++ )
	{
		fprintf( f, "%s=%s\n", set_cfg.key_name[id], set_cfg.key_value[id] );
	}
	return fclose( f ) == 0 ? 0 : -1;
}

/**
  * @brief  A changed key assignment.
  *
  * @param  name "key.<control>" or "key2.<control>".
  *
  * @retval The key's name ("" = none), NULL if not changed (the default applies).
  */
const char *set_key_get( const char *name )
{
	int i;

	for ( i = 0; i < set_cfg.nkeys; i++ )
	{
		if ( !strcmp( set_cfg.key_name[i], name ) )
		{
			return set_cfg.key_value[i];
		}
	}
	return NULL;
}

/**
  * @brief  Store a key assignment (call set_key_reset first to replace them all).
  *
  * @param  name "key.<control>" or "key2.<control>".
  * @param  value The key's name, "" for none.
  *
  * @retval 0 stored, -1 no room.
  */
int set_key_put( const char *name, const char *value )
{
	int i;

	for ( i = 0; i < set_cfg.nkeys && strcmp( set_cfg.key_name[i], name ); i++ )
	{
	}
	if ( i == SET_KEYS || strlen( name ) >= SET_KEYN || strlen( value ) >= SET_KEYN )
	{
		return -1;
	}
	snprintf( set_cfg.key_name[i], SET_KEYN, "%s", name );
	snprintf( set_cfg.key_value[i], SET_KEYN, "%s", value );
	if ( i == set_cfg.nkeys )
	{
		set_cfg.nkeys++;
	}
	return 0;
}

/**
  * @brief  Forget every changed key assignment (the defaults apply).
  *
  * @retval None.
  */
void set_key_reset( void )
{
	set_cfg.nkeys = 0;
}
