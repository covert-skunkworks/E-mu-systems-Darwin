//  Description:
//
//		settings.h -- Emu_Darwin.ini next to the program.
//
//		Plain "key=value" lines; '#' or ';' starts a comment. Keys:
//
//		  audio0 .. audio7   disk image on the audio SCSI bus at that ID
//		  host0 .. host7     disk image on the host SCSI bus at that ID
//		  debug_duart        1 (default): fit the debug DUART at B00000
//		  midi_in, midi_out  host MIDI devices on the Darwin's MIDI in / out
//		                     ("" = none)
//		  ui_half            1: open the panel window at half its usual size
//		  phones_volume      the headphone VOLUME knob, 0-100 (default 100)
//		  fast_panel         1: run the front-panel link 16 times faster than
//		                     the hardware's 31250 baud (menus redraw at once)
//		  keyboard           1 (default): the computer keyboard works the
//		                     panel keys (the list is on the Keyboard tab)
//		  firmware           the firmware zip to run, by file name (e.g.
//		                     emu_darwin_v2_02.zip); "" = the newest found
//		  audio_out          host playback device ("" = the Windows default)
//		  audio_in           host recording device ("" = the Windows default,
//		                     "-" = none: the inputs are silent)
//		  in1 .. in10        host input channel (1 = first) feeding the
//		                     Darwin's input: 1-8 analog (5-8 on the input
//		                     expander, which the firmware finds fitted),
//		                     9 / 10 S/PDIF L / R; 0 = silent
//		  out1 .. out12      host output channel carrying the Darwin's
//		                     output: 1-8 the track outputs, 9 / 10 the
//		                     headphones L / R, 11 / 12 S/PDIF out L / R
//		                     (both carry the 8 x 2 mix); 0 = not heard
//		  key.NAME, key2.NAME  the keyboard key / alternate key of a panel
//		                     control (gui.c's names, e.g. key.PLAY=Space);
//		                     only the changed ones are stored, "" = none
//
//		The Darwin's own settings are not here: they live in its NVRAM
//		(darwin_nvram_<version>.bin, one per firmware, rtc.c).
//

#ifndef SETTINGS_H
#define SETTINGS_H

#define SET_PATH 1024  // a path (Linux and macOS allow long ones)
#define SET_DEV  260   // device name
#define SET_KEYS 128   // key assignments kept (two per panel control)
#define SET_KEYN 32    // room for a key setting's name or value
#define SET_INS  10    // Darwin inputs: analog 1-8, S/PDIF L, R
#define SET_OUTS 12    // Darwin outputs: tracks 1-8, phones L, R, S/PDIF out L, R
#define SET_CH   16    // host channels offered per device

typedef struct
{
	char disk[2][8][SET_PATH];  // [bus][id]: 0 host, 1 audio; "" = none
	int  debug_duart;
	int  fast_panel;            // the panel link at 16 x its baud rate (snappier, not authentic)
	int  ui_half;               // the panel window at half its usual size
	int  keyboard;              // the computer keyboard works the panel keys
	char firmware[SET_DEV];     // firmware zip file name to run, "" = the newest found
	int  phones_volume;         // the headphone VOLUME knob, 0-100 (100 = full)
	char midi_in[SET_DEV];      // host MIDI input device into the Darwin's MIDI in, "" = none
	char midi_out[SET_DEV];     // host MIDI output device for the Darwin's MIDI out, "" = none
	char audio_out[SET_DEV];
	char audio_in[SET_DEV];
	int  in_ch[SET_INS];        // host channel per Darwin input, 1-based; 0 = silent
	int  out_ch[SET_OUTS];      // host channel per Darwin output, 1-based; 0 = not heard
	char key_name[SET_KEYS][SET_KEYN];  // changed key assignments: "key.PLAY" ...
	char key_value[SET_KEYS][SET_KEYN];  // ... and the key's name ("" = none)
	int  nkeys;
} set_config;

extern set_config set_cfg;

extern const char *const set_in_names[SET_INS];
extern const char *const set_out_names[SET_OUTS];

/**
  * @brief  Read the settings file (missing file: defaults).
  *
  * @param  path The file.
  *
  * @retval 0 read, 1 no file.
  */
int set_load( const char *path );

/**
  * @brief  Write the settings file.
  *
  * @retval 0 written, -1 failed.
  */
int set_save( void );

/**
  * @brief  A changed key assignment.
  *
  * @param  name "key.<control>" or "key2.<control>".
  *
  * @retval The key's name ("" = none), NULL if not changed (the default applies).
  */
const char *set_key_get( const char *name );

/**
  * @brief  Store a key assignment (call set_key_reset first to replace them all).
  *
  * @param  name "key.<control>" or "key2.<control>".
  * @param  value The key's name, "" for none.
  *
  * @retval 0 stored, -1 no room.
  */
int set_key_put( const char *name, const char *value );

/**
  * @brief  Forget every changed key assignment (the defaults apply).
  *
  * @retval None.
  */
void set_key_reset( void );

#endif
