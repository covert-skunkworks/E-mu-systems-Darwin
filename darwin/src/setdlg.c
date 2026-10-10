//  Description:
//
//		setdlg.c -- the settings window, opened from the E-MU logo on the panel.
//
//		A window of its own, drawn with Nuklear (nk.h) through SDL, so it is
//		the same on Windows, Linux and macOS. Six pages:
//
//		  Audio I/O  the host playback / recording devices, and the host
//		             channel for each Darwin input and output
//		             (settings.h: in1-10, out1-12)
//		  MIDI       host MIDI devices on the Darwin's MIDI in / out
//		  Disks      disk images on the audio SCSI bus (IDs 0-6)
//		  Options    debug DUART, fast panel link, window size, firmware
//		  Keyboard   the computer keyboard on or off, and the key of every
//		             panel control (gui.c's table)
//		  About      version, firmware, the independence notice
//
//		The window is modal: while it is open the panel takes no input, but
//		the emulator runs on and the panel is redrawn each frame. Everything
//		is edited in a working copy and applied on OK.
//
//		Files: Windows shows its own file dialogs; elsewhere a small file
//		browser in the same window (or, from the panel's drive bay, in a
//		window of its own) lists folders and disk images.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "setdlg.h"
#include "darwin.h"
#include "gui.h"
#include "machine.h"
#include "midi.h"
#include "nk.h"
#include "plat.h"
#include "scsidisk.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <SDL2/SDL_syswm.h>
#endif

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define SDLG_W      720   // window size
#define SDLG_H      640
#define SDLG_PAGES  6
#define SDLG_IDS    7     // audio SCSI IDs offered (7 is the Darwin)
#define SDLG_KEYS   96    // panel controls the Keyboard page can list
#define SDLG_DEVS   32    // host audio devices listed per direction
#define SDLG_FW     16    // firmware zips listed
#define SDLG_ROW    26.0f // a row of controls
#define SDLG_FONT   16.0f // text height in pixels

enum
{
	SDLG_AUDIO, SDLG_MIDI, SDLG_DISKS, SDLG_OPTIONS, SDLG_KEYBOARD, SDLG_ABOUT
};

//------------------------------------------------------------------------------
//---------------------------- Local types and structures ----------------------
//------------------------------------------------------------------------------

// an entry of the file browser
typedef struct
{
	char name[256];
	int  dir;
} sdlg_entry;

// the file browser
typedef struct
{
	int         active;
	int         save;             // naming a new file
	int         target;           // -1 the caller of sdlg_pick_file; 0-6 a disk row; 100 + id a new image
	int         done, ok;
	nk_bool     all;              // every file, not only disk images
	char        dir[PLAT_PATH];   // the folder shown, ending in a separator
	char        name[PLAT_PATH];  // the file chosen in it
	char        title[128];
	sdlg_entry *e;
	int         n, cap;
} sdlg_browser;

// the window's working copy of the settings, and its lists
typedef struct
{
	int     page;
	int     out_dev, in_dev;           // device list selections
	int     in_ch[SET_INS], out_ch[SET_OUTS];
	int     midi_in, midi_out;
	char    disk[SDLG_IDS][PLAT_PATH];
	char    newsize[16];
	nk_bool debug, fast, keyboard;
	int     half, fw;
	int32_t kw[SDLG_KEYS][2];          // key, alternate key per panel control
	int     nkeys;
	int     cap_ctl, cap_slot;         // waiting for a key (cap_slot -1: not)
	int     key_sel;                   // the selected control
	int     info_out, info_in;         // channel counts of the chosen devices
	int     info_sel[2];               // the selections they were found for

	char        dev_names[2][SDLG_DEVS + 2][128];  // [capture]: the list's entries
	const char *dev_items[2][SDLG_DEVS + 2];
	int         ndev[2];
	char        midi_names[2][34][MIDI_NAME];      // [out]: "None", the host's devices
	const char *midi_items[2][34];
	int         nmidi[2];
	char        fw_names[SDLG_FW][DW_FW_NAME];
	char        fw_text[SDLG_FW + 1][DW_FW_NAME + 16];
	const char *fw_items[SDLG_FW + 1];
	int         nfw;
} sdlg_state;

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

extern const unsigned char font_ttf[];  // DroidSans (Nuklear's extra fonts), built in by tools/bin2c.c
extern const unsigned long font_ttf_len;

static const char *const sdlg_page_names[SDLG_PAGES] = { "Audio I/O", "MIDI", "Disks", "Options", "Keyboard",
														   "About" };
static const char *const sdlg_channels[SET_CH + 1] = { "Off", "Channel 1", "Channel 2", "Channel 3", "Channel 4",
														"Channel 5", "Channel 6", "Channel 7", "Channel 8",
														"Channel 9", "Channel 10", "Channel 11", "Channel 12",
														"Channel 13", "Channel 14", "Channel 15", "Channel 16" };

static SDL_Window         *sdlg_win;
static SDL_Renderer       *sdlg_ren;
static struct nk_context  *sdlg_ctx;
static sdlg_state          sdlg_st;
static sdlg_browser        sdlg_br;
static int                 sdlg_done, sdlg_ok, sdlg_quit;
static int                 sdlg_last_page;
static int                 sdlg_resize;   // the window size setting changed: resize when the window has closed
static const char         *sdlg_shot;     // -dlgshot: capture each page to BMP files, then close
static int                 sdlg_frames;

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void sdlg_text_color( struct nk_style *s, struct nk_color c );
static int sdlg_window_open( const char *title, int w, int h );
static void sdlg_window_close( void );
static Uint32 sdlg_event_window( const SDL_Event *ev );
static int sdlg_pump( void );
static void sdlg_render( int shot_page );
static void sdlg_row( float h, int cols, const float *ratio );
static void sdlg_text( const char *t );
static void sdlg_message( const char *title, const char *msg );
static int sdlg_device_channels( int capture, int sel );
static void sdlg_load( void );
static void sdlg_accept( void );
static void sdlg_page_audio( void );
static void sdlg_page_midi( void );
static void sdlg_page_disks( void );
static void sdlg_page_options( void );
static void sdlg_page_keys( void );
static void sdlg_page_about( void );
static void sdlg_key_set( int i, int slot, int32_t k );
static void sdlg_ui( int w, int h );
static void sdlg_disks_dir( char *out, size_t n );
static int sdlg_native_pick( const char *title, char *path, size_t n, int save );
static int sdlg_entry_cmp( const void *a, const void *b );
static void sdlg_br_add( const char *name, int is_dir, void *ctx );
static void sdlg_br_list( void );
static void sdlg_br_start( const char *title, const char *path, int save, int target );
static void sdlg_br_ui( float h );
static void sdlg_br_finish( void );
static void sdlg_choose( int target, int save, const char *title );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  One colour for the text of every kind of control.
  *
  * Nuklear's own grey (175) is dim on its dark background; each control
  * type keeps its text colours apart, so they are all set here.
  *
  * @param  s The style.
  * @param  c The colour.
  *
  * @retval None.
  */
static void sdlg_text_color( struct nk_style *s, struct nk_color c )
{
	s->text.color = c;
	s->button.text_normal = s->button.text_hover = s->button.text_active = c;
	s->checkbox.text_normal = s->checkbox.text_hover = s->checkbox.text_active = c;
	s->selectable.text_normal = s->selectable.text_hover = s->selectable.text_pressed = c;
	s->selectable.text_normal_active = s->selectable.text_hover_active = s->selectable.text_pressed_active = c;
	s->combo.label_normal = s->combo.label_hover = s->combo.label_active = c;
	s->edit.text_normal = s->edit.text_hover = s->edit.text_active = c;
}

/**
  * @brief  Open the window and start Nuklear in it, with the built-in font.
  *
  * @param  title The title.
  * @param  w Width.
  * @param  h Height.
  *
  * @retval 0 open, -1 failed.
  */
static int sdlg_window_open( const char *title, int w, int h )
{
	struct nk_font_atlas *atlas;
	struct nk_font *font;

	sdlg_win = SDL_CreateWindow( title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_SHOWN );
	if ( !sdlg_win )
	{
		return -1;
	}
	sdlg_ren = SDL_CreateRenderer( sdlg_win, -1, SDL_RENDERER_ACCELERATED );
	if ( !sdlg_ren )
	{
		sdlg_ren = SDL_CreateRenderer( sdlg_win, -1, SDL_RENDERER_SOFTWARE );
	}
	if ( !sdlg_ren )
	{
		SDL_DestroyWindow( sdlg_win );
		sdlg_win = NULL;
		return -1;
	}
	sdlg_ctx = nk_sdl_init( sdlg_win, sdlg_ren );
	nk_sdl_font_stash_begin( &atlas );
	font = nk_font_atlas_add_from_memory( atlas, (void *)font_ttf, (nk_size)font_ttf_len, SDLG_FONT, NULL );
	nk_sdl_font_stash_end();
	if ( font )
	{
		nk_style_set_font( sdlg_ctx, &font->handle );
	}
	sdlg_ctx->style.window.padding = nk_vec2( 10, 8 );
	sdlg_ctx->style.window.group_padding = nk_vec2( 10, 8 );
	// the default theme marks a ticked box with a dark square on grey, which reads as empty:
	// a dark box with an amber mark (the panel's LED colour) instead
	sdlg_ctx->style.checkbox.normal = nk_style_item_color( nk_rgb( 30, 30, 30 ) );
	sdlg_ctx->style.checkbox.hover = nk_style_item_color( nk_rgb( 40, 40, 40 ) );
	sdlg_ctx->style.checkbox.active = nk_style_item_color( nk_rgb( 40, 40, 40 ) );
	sdlg_ctx->style.checkbox.cursor_normal = nk_style_item_color( nk_rgb( 255, 176, 32 ) );
	sdlg_ctx->style.checkbox.cursor_hover = nk_style_item_color( nk_rgb( 255, 200, 90 ) );
	sdlg_ctx->style.checkbox.border_color = nk_rgb( 140, 140, 140 );
	sdlg_ctx->style.checkbox.border = 1.0f;
	sdlg_text_color( &sdlg_ctx->style, nk_rgb( 235, 235, 235 ) );
	SDL_RaiseWindow( sdlg_win );
	return 0;
}

/**
  * @brief  Close the window.
  *
  * @retval None.
  */
static void sdlg_window_close( void )
{
	nk_sdl_shutdown();
	SDL_DestroyRenderer( sdlg_ren );
	SDL_DestroyWindow( sdlg_win );
	sdlg_ren = NULL;
	sdlg_win = NULL;
	sdlg_ctx = NULL;
}

/**
  * @brief  The window an event belongs to.
  *
  * @param  ev The event.
  *
  * @retval Its window ID, 0 for events of no window.
  */
static Uint32 sdlg_event_window( const SDL_Event *ev )
{
	switch ( ev->type )
	{
	case SDL_WINDOWEVENT: return ev->window.windowID;
	case SDL_KEYDOWN:
	case SDL_KEYUP: return ev->key.windowID;
	case SDL_TEXTINPUT: return ev->text.windowID;
	case SDL_TEXTEDITING: return ev->edit.windowID;
	case SDL_MOUSEMOTION: return ev->motion.windowID;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP: return ev->button.windowID;
	case SDL_MOUSEWHEEL: return ev->wheel.windowID;
	default: return 0;
	}
}

/**
  * @brief  Take the waiting events: the window's go to Nuklear (or a key being
  *         assigned); the panel's are dropped, as a modal window does.
  *
  * @retval 1 the window was closed (or the program asked to quit), 0 not.
  */
static int sdlg_pump( void )
{
	Uint32 id = SDL_GetWindowID( sdlg_win );
	SDL_Event ev;
	int closed = 0;

	nk_input_begin( sdlg_ctx );
	while ( SDL_PollEvent( &ev ) )
	{
		if ( ev.type == SDL_QUIT )
		{
			sdlg_quit = 1;  // handed back to the panel once the window has closed
			closed = 1;
			continue;
		}
		if ( sdlg_event_window( &ev ) != id )
		{
			continue;
		}
		if ( ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE )
		{
			closed = 1;
			continue;
		}
		if ( sdlg_st.cap_slot >= 0 && ( ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP || ev.type == SDL_TEXTINPUT ) )
		{
			if ( ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym )
			{
				sdlg_key_set( sdlg_st.cap_ctl, sdlg_st.cap_slot, ev.key.keysym.sym );
				sdlg_st.cap_slot = -1;
			}
			continue;  // the key is taken, whatever it is (Esc, Enter and Tab included)
		}
		nk_sdl_handle_event( &ev );
	}
	nk_input_end( sdlg_ctx );
	return closed;
}

/**
  * @brief  Draw what Nuklear built and show it (and for -dlgshot, save it first).
  *
  * @param  shot_page The page to save as <sdlg_shot><page>.bmp, -1 for none.
  *
  * @retval None.
  */
static void sdlg_render( int shot_page )
{
	char path[PLAT_PATH];
	SDL_Surface *s;
	int w, h;

	SDL_SetRenderDrawColor( sdlg_ren, 45, 45, 45, 255 );
	SDL_RenderClear( sdlg_ren );
	nk_sdl_render( NK_ANTI_ALIASING_ON );
	if ( shot_page >= 0 && SDL_GetRendererOutputSize( sdlg_ren, &w, &h ) == 0 &&
		 ( s = SDL_CreateRGBSurfaceWithFormat( 0, w, h, 32, SDL_PIXELFORMAT_ARGB8888 ) ) )
	{
		if ( SDL_RenderReadPixels( sdlg_ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch ) == 0 )
		{
			snprintf( path, sizeof path, "%s%d.bmp", sdlg_shot, shot_page );
			SDL_SaveBMP( s, path );
		}
		SDL_FreeSurface( s );
	}
	SDL_RenderPresent( sdlg_ren );
}

/**
  * @brief  Start a row of controls.
  *
  * @param  h Height.
  * @param  cols Columns.
  * @param  ratio Their shares of the width (NULL: equal).
  *
  * @retval None.
  */
static void sdlg_row( float h, int cols, const float *ratio )
{
	if ( ratio )
	{
		nk_layout_row( sdlg_ctx, NK_DYNAMIC, h, cols, ratio );
	}
	else
	{
		nk_layout_row_dynamic( sdlg_ctx, h, cols );
	}
}

/**
  * @brief  A line of text across the page.
  *
  * @param  t The text.
  *
  * @retval None.
  */
static void sdlg_text( const char *t )
{
	sdlg_row( 20, 1, NULL );
	nk_label( sdlg_ctx, t, NK_TEXT_LEFT );
}

/**
  * @brief  A message box over the window.
  *
  * @param  title The title.
  * @param  msg The message.
  *
  * @retval None.
  */
static void sdlg_message( const char *title, const char *msg )
{
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_WARNING, title, msg, sdlg_win );
}

/**
  * @brief  Channels of the device chosen in a device list.
  *
  * @param  capture 1 for the recording list.
  * @param  sel The list's selection.
  *
  * The system default device's channels need SDL 2.24 (SDL_GetDefaultAudioInfo); with an older
  * SDL (Ubuntu 22.04 has 2.0.20) they show as unknown.
  *
  * @retval Channels, 0 if unknown (or none chosen).
  */
static int sdlg_device_channels( int capture, int sel )
{
	SDL_AudioSpec spec;
	int first = capture ? 2 : 1;  // entries before the device names

	memset( &spec, 0, sizeof spec );
	if ( sel == first - 1 )  // the system's default
	{
#if SDL_VERSION_ATLEAST( 2, 24, 0 )
		char *name = NULL;

		if ( SDL_GetDefaultAudioInfo( &name, &spec, capture ) != 0 )
		{
			return 0;
		}
		SDL_free( name );
		return spec.channels;
#else
		return 0;
#endif
	}
	if ( sel < first || SDL_GetAudioDeviceSpec( sel - first, capture, &spec ) != 0 )
	{
		return 0;
	}
	return spec.channels;
}

/**
  * @brief  Fill the working copy and the lists from the settings.
  *
  * @retval None.
  */
static void sdlg_load( void )
{
	sdlg_state *s = &sdlg_st;
	const char *name;
	int c, i, n;

	memset( s, 0, sizeof *s );
	s->page = sdlg_last_page;
	for ( c = 0; c < 2; c++ )  // audio devices: [0] playback, [1] recording
	{
		s->ndev[c] = 0;
		if ( c )
		{
			snprintf( s->dev_names[c][s->ndev[c]++], 128, "None (the inputs are silent)" );
		}
		snprintf( s->dev_names[c][s->ndev[c]++], 128, c ? "System default recording device" :
															"System default playback device" );
		n = SDL_GetNumAudioDevices( c );
		for ( i = 0; i < n && s->ndev[c] < SDLG_DEVS + 2; i++ )
		{
			name = SDL_GetAudioDeviceName( i, c );
			snprintf( s->dev_names[c][s->ndev[c]++], 128, "%s", name ? name : "?" );
		}
		for ( i = 0; i < s->ndev[c]; i++ )
		{
			s->dev_items[c][i] = s->dev_names[c][i];
		}
	}
	s->out_dev = 0;
	s->in_dev = !strcmp( set_cfg.audio_in, "-" ) ? 0 : 1;
	for ( i = 1; i < s->ndev[0]; i++ )
	{
		s->out_dev = set_cfg.audio_out[0] && !strcmp( s->dev_names[0][i], set_cfg.audio_out ) ? i : s->out_dev;
	}
	for ( i = 2; i < s->ndev[1]; i++ )
	{
		s->in_dev = set_cfg.audio_in[0] && !strcmp( s->dev_names[1][i], set_cfg.audio_in ) ? i : s->in_dev;
	}
	s->info_sel[0] = s->info_sel[1] = -1;
	memcpy( s->in_ch, set_cfg.in_ch, sizeof s->in_ch );
	memcpy( s->out_ch, set_cfg.out_ch, sizeof s->out_ch );

	for ( c = 0; c < 2; c++ )  // MIDI: [0] in, [1] out
	{
		const char *cur = c ? set_cfg.midi_out : set_cfg.midi_in;
		int *sel = c ? &s->midi_out : &s->midi_in;

		s->nmidi[c] = 0;
		snprintf( s->midi_names[c][s->nmidi[c]++], MIDI_NAME, "None" );
		*sel = 0;
		for ( i = 0; i < midi_dev_count( c ) && s->nmidi[c] < 33; i++ )
		{
			if ( midi_dev_name( c, i, s->midi_names[c][s->nmidi[c]] ) == 0 )
			{
				*sel = cur[0] && !strcmp( s->midi_names[c][s->nmidi[c]], cur ) ? s->nmidi[c] : *sel;
				s->nmidi[c]++;
			}
		}
		if ( cur[0] && !*sel )  // configured but not plugged in: keep it
		{
			snprintf( s->midi_names[c][s->nmidi[c]], MIDI_NAME, "%.*s", MIDI_NAME - 1, cur );
			*sel = s->nmidi[c]++;
		}
		for ( i = 0; i < s->nmidi[c]; i++ )
		{
			s->midi_items[c][i] = s->midi_names[c][i];
		}
	}

	for ( i = 0; i < SDLG_IDS; i++ )
	{
		snprintf( s->disk[i], sizeof s->disk[i], "%s", set_cfg.disk[1][i] );
	}
	snprintf( s->newsize, sizeof s->newsize, "1000" );
	s->debug = set_cfg.debug_duart != 0;
	s->fast = set_cfg.fast_panel != 0;
	s->half = set_cfg.ui_half != 0;
	s->keyboard = set_cfg.keyboard != 0;

	s->nfw = dw_firmware_list( s->fw_names, SDLG_FW );
	snprintf( s->fw_text[0], sizeof s->fw_text[0], "Newest found" );
	s->fw_items[0] = s->fw_text[0];
	s->fw = 0;
	for ( i = 0; i < s->nfw; i++ )
	{
		snprintf( s->fw_text[i + 1], sizeof s->fw_text[i + 1], "%.*s%s", DW_FW_NAME - 1, s->fw_names[i],
				  plat_stricmp( s->fw_names[i], dw_fw_zip ) ? "" : "  (running)" );
		s->fw_items[i + 1] = s->fw_text[i + 1];
		s->fw = !plat_stricmp( s->fw_names[i], set_cfg.firmware ) ? i + 1 : s->fw;
	}

	gui_keys_load();
	s->nkeys = gui_key_controls() < SDLG_KEYS ? gui_key_controls() : SDLG_KEYS;
	for ( i = 0; i < s->nkeys; i++ )
	{
		s->kw[i][0] = gui_key_current( i, 0 );
		s->kw[i][1] = gui_key_current( i, 1 );
	}
	s->cap_slot = -1;
	s->key_sel = -1;
}

/**
  * @brief  OK: copy the working copy into the settings, apply the disks, save.
  *
  * @retval None.
  */
static void sdlg_accept( void )
{
	sdlg_state *s = &sdlg_st;
	char p[SET_PATH];
	int i, slot, fw_changed;

	snprintf( set_cfg.audio_out, SET_DEV, "%s", s->out_dev >= 1 ? s->dev_names[0][s->out_dev] : "" );
	snprintf( set_cfg.audio_in, SET_DEV, "%s", s->in_dev == 0 ? "-" : s->in_dev >= 2 ? s->dev_names[1][s->in_dev] : "" );
	memcpy( set_cfg.in_ch, s->in_ch, sizeof set_cfg.in_ch );
	memcpy( set_cfg.out_ch, s->out_ch, sizeof set_cfg.out_ch );
	for ( i = 0; i < SDLG_IDS; i++ )
	{
		if ( !strcmp( s->disk[i], set_cfg.disk[1][i] ) )
		{
			continue;
		}
		snprintf( p, sizeof p, "%s", s->disk[i] );
		// the bus changes at once, like a drive plugged in or out
		dw_lock();
		sd_detach( 1, i );
		if ( p[0] && sd_attach( 1, i, p ) != 0 )
		{
			dw_unlock();
			sdlg_message( "Disks", "A disk image could not be opened; that ID is left empty." );
			p[0] = 0;
			dw_lock();
		}
		dw_unlock();
		snprintf( set_cfg.disk[1][i], SET_PATH, "%s", p );
	}
	set_cfg.debug_duart = s->debug != 0;
	set_cfg.fast_panel = s->fast != 0;
	set_cfg.keyboard = s->keyboard != 0;
	sdlg_resize = s->half != set_cfg.ui_half;
	set_cfg.ui_half = s->half;
	fw_changed = strcmp( s->fw >= 1 ? s->fw_names[s->fw - 1] : "", set_cfg.firmware ) != 0;
	snprintf( set_cfg.firmware, SET_DEV, "%s", s->fw >= 1 ? s->fw_names[s->fw - 1] : "" );
	set_key_reset();  // only what differs from the defaults is kept
	for ( i = 0; i < s->nkeys; i++ )
	{
		for ( slot = 0; slot < 2; slot++ )
		{
			if ( s->kw[i][slot] != gui_key_default( i, slot ) )
			{
				snprintf( p, sizeof p, "%s.%s", slot ? "key2" : "key", gui_key_control( i ) );
				set_key_put( p, s->kw[i][slot] ? SDL_GetKeyName( s->kw[i][slot] ) : "" );
			}
		}
	}
	snprintf( set_cfg.midi_in, SET_DEV, "%s", s->midi_in >= 1 ? s->midi_names[0][s->midi_in] : "" );
	snprintf( set_cfg.midi_out, SET_DEV, "%s", s->midi_out >= 1 ? s->midi_names[1][s->midi_out] : "" );
	dw_lock();
	mc_duart.ch[0].fast = set_cfg.fast_panel;  // channel A is the panel link
	if ( midi_open( set_cfg.midi_in, set_cfg.midi_out ) != 0 )  // under the lock: MIDI out runs in the emulator
	{
		dw_unlock();
		sdlg_message( "MIDI", "A MIDI device could not be opened (in use by another program?)." );
		dw_lock();
	}
	dw_unlock();
	set_save();
	if ( fw_changed )
	{
		dw_firmware_request();  // the emulator restarts the unit with it
	}
}

/**
  * @brief  The Audio I/O page.
  *
  * @retval None.
  */
static void sdlg_page_audio( void )
{
	static const float dev[] = { 0.2f, 0.6f, 0.2f }, io[] = { 0.24f, 0.22f, 0.04f, 0.28f, 0.22f };
	sdlg_state *s = &sdlg_st;
	char b[32];
	int c, i;

	for ( c = 0; c < 2; c++ )
	{
		int *sel = c ? &s->in_dev : &s->out_dev, *info = c ? &s->info_in : &s->info_out;

		sdlg_row( SDLG_ROW, 3, dev );
		nk_label( sdlg_ctx, c ? "Recording device:" : "Playback device:", NK_TEXT_LEFT );
		*sel = nk_combo( sdlg_ctx, s->dev_items[c], s->ndev[c], *sel, 22, nk_vec2( 420, 300 ) );
		if ( s->info_sel[c] != *sel )
		{
			s->info_sel[c] = *sel;
			*info = sdlg_device_channels( c, *sel );
		}
		snprintf( b, sizeof b, "%d channel%s", *info, *info == 1 ? "" : "s" );
		nk_label( sdlg_ctx, *info > 0 ? b : "", NK_TEXT_LEFT );
	}
	sdlg_row( 8, 1, NULL );
	sdlg_row( 20, 5, io );
	nk_label( sdlg_ctx, "Darwin input", NK_TEXT_LEFT );
	nk_label( sdlg_ctx, "from channel", NK_TEXT_LEFT );
	nk_spacing( sdlg_ctx, 1 );
	nk_label( sdlg_ctx, "Darwin output", NK_TEXT_LEFT );
	nk_label( sdlg_ctx, "to channel", NK_TEXT_LEFT );
	for ( i = 0; i < SET_OUTS; i++ )
	{
		sdlg_row( SDLG_ROW, 5, io );
		if ( i < SET_INS )
		{
			nk_label( sdlg_ctx, set_in_names[i], NK_TEXT_LEFT );
			s->in_ch[i] = nk_combo( sdlg_ctx, sdlg_channels, SET_CH + 1, s->in_ch[i], 22, nk_vec2( 140, 260 ) );
		}
		else
		{
			nk_spacing( sdlg_ctx, 2 );
		}
		nk_spacing( sdlg_ctx, 1 );
		nk_label( sdlg_ctx, set_out_names[i], NK_TEXT_LEFT );
		s->out_ch[i] = nk_combo( sdlg_ctx, sdlg_channels, SET_CH + 1, s->out_ch[i], 22, nk_vec2( 140, 260 ) );
	}
	sdlg_row( 8, 1, NULL );
	sdlg_text( "Device changes take effect at the next power-on; the channel routing at once." );
}

/**
  * @brief  The MIDI page.
  *
  * @retval None.
  */
static void sdlg_page_midi( void )
{
	static const float r[] = { 0.2f, 0.8f };
	sdlg_state *s = &sdlg_st;

	sdlg_text( "The Darwin's MIDI port: MIDI Machine Control and MIDI Time Code out, MMC and" );
	sdlg_text( "MTC in (System > MIDI on the Darwin sets what it sends and answers)." );
	sdlg_row( 10, 1, NULL );
	sdlg_row( SDLG_ROW, 2, r );
	nk_label( sdlg_ctx, "MIDI in from:", NK_TEXT_LEFT );
	s->midi_in = nk_combo( sdlg_ctx, s->midi_items[0], s->nmidi[0], s->midi_in, 22, nk_vec2( 480, 300 ) );
	sdlg_row( SDLG_ROW, 2, r );
	nk_label( sdlg_ctx, "MIDI out to:", NK_TEXT_LEFT );
	s->midi_out = nk_combo( sdlg_ctx, s->midi_items[1], s->nmidi[1], s->midi_out, 22, nk_vec2( 480, 300 ) );
	sdlg_row( 10, 1, NULL );
	sdlg_text( "Changes take effect when you press OK." );
}

/**
  * @brief  The Disks page.
  *
  * @retval None.
  */
static void sdlg_page_disks( void )
{
	static const float r[] = { 0.13f, 0.53f, 0.07f, 0.12f, 0.15f }, sz[] = { 0.34f, 0.14f, 0.52f };
	sdlg_state *s = &sdlg_st;
	char b[32];
	long mb;
	int i;

	sdlg_text( "Disk images on the audio SCSI bus. The Darwin finds a changed drive with" );
	sdlg_text( "System > Disk > Mount (or at the next power-on). It formats 275 MB and up, 2 GB at most." );
	sdlg_row( 8, 1, NULL );
	for ( i = 0; i < SDLG_IDS; i++ )
	{
		snprintf( b, sizeof b, "SCSI ID %d:", i );
		sdlg_row( SDLG_ROW, 5, r );
		nk_label( sdlg_ctx, b, NK_TEXT_LEFT );
		nk_edit_string_zero_terminated( sdlg_ctx, NK_EDIT_FIELD, s->disk[i], (int)sizeof s->disk[i], nk_filter_default );
		if ( nk_button_label( sdlg_ctx, "..." ) )
		{
			sdlg_choose( i, 0, "Disk image" );
		}
		if ( nk_button_label( sdlg_ctx, "New..." ) )
		{
			mb = atol( s->newsize );
			if ( mb < 1 || mb > 8192 )
			{
				sdlg_message( "New disk image", "Enter the size of the new image, 1 .. 8192 MB." );
			}
			else
			{
				sdlg_choose( 100 + i, 1, "New blank disk image" );
			}
		}
		if ( nk_button_label( sdlg_ctx, "Remove" ) )
		{
			s->disk[i][0] = 0;
		}
	}
	sdlg_row( 8, 1, NULL );
	sdlg_row( SDLG_ROW, 3, sz );
	nk_label( sdlg_ctx, "Size of a new image (MB):", NK_TEXT_LEFT );
	nk_edit_string_zero_terminated( sdlg_ctx, NK_EDIT_FIELD, s->newsize, (int)sizeof s->newsize, nk_filter_decimal );
	nk_label( sdlg_ctx, "New images are sparse: they grow as they are written.", NK_TEXT_LEFT );
}

/**
  * @brief  The Options page.
  *
  * @retval None.
  */
static void sdlg_page_options( void )
{
	static const char *const sizes[] = { "Original", "Half (1/2)" };
	static const float r[] = { 0.25f, 0.6f, 0.15f };
	sdlg_state *s = &sdlg_st;

	sdlg_row( SDLG_ROW, 1, NULL );
	nk_checkbox_label( sdlg_ctx, "Debug DUART at B00000 (the boot log; a development unit)", &s->debug );
	sdlg_text( "     Takes effect at the next power-on." );
	sdlg_row( 10, 1, NULL );
	sdlg_row( SDLG_ROW, 1, NULL );
	nk_checkbox_label( sdlg_ctx, "Fast front-panel link (16 x the real 31250 baud)", &s->fast );
	sdlg_text( "     A menu change sends about 1 KB to the panel: half a second at the real rate," );
	sdlg_text( "     as on the hardware. Faster than any real Darwin; takes effect at once." );
	sdlg_row( 14, 1, NULL );
	sdlg_row( SDLG_ROW, 3, r );
	nk_label( sdlg_ctx, "Panel window size:", NK_TEXT_LEFT );
	s->half = nk_combo( sdlg_ctx, sizes, 2, s->half, 22, nk_vec2( 300, 100 ) );
	nk_spacing( sdlg_ctx, 1 );
	sdlg_text( "     The window is resized at once; it can still be dragged to any size." );
	sdlg_row( 14, 1, NULL );
	sdlg_row( SDLG_ROW, 3, r );
	nk_label( sdlg_ctx, "Firmware:", NK_TEXT_LEFT );
	s->fw = nk_combo( sdlg_ctx, s->fw_items, s->nfw + 1, s->fw, 22, nk_vec2( 360, 200 ) );
	nk_spacing( sdlg_ctx, 1 );
	sdlg_text( "     A change restarts the Darwin with that firmware, as a power cycle. Each firmware" );
	sdlg_text( "     keeps its own settings memory (darwin_nvram_<version>.bin); disks work with both." );
}

/**
  * @brief  Give a control a key in the working copy; a key used elsewhere moves here.
  *
  * @param  i Control.
  * @param  slot 0 the key, 1 the alternate key.
  * @param  k SDL key code, 0 for none.
  *
  * @retval None.
  */
static void sdlg_key_set( int i, int slot, int32_t k )
{
	int j, sl;

	for ( j = 0; k && j < sdlg_st.nkeys; j++ )
	{
		for ( sl = 0; sl < 2; sl++ )
		{
			if ( sdlg_st.kw[j][sl] == k && ( j != i || sl != slot ) )
			{
				sdlg_st.kw[j][sl] = 0;
			}
		}
	}
	sdlg_st.kw[i][slot] = k;
}

/**
  * @brief  The Keyboard page: the switch, and every control with its two keys.
  *
  * Clicking a key starts waiting for the new key; clicking it again stops.
  *
  * @retval None.
  */
static void sdlg_page_keys( void )
{
	static const float r[] = { 0.4f, 0.3f, 0.3f }, btn[] = { 0.2f, 0.25f, 0.55f };
	sdlg_state *s = &sdlg_st;
	const char *t;
	nk_bool on;
	int i, slot;

	sdlg_row( SDLG_ROW, 1, NULL );
	nk_checkbox_label( sdlg_ctx, "Computer keyboard shortcuts for the panel keys", &s->keyboard );
	sdlg_row( 20, 3, r );
	nk_label( sdlg_ctx, "Panel control", NK_TEXT_LEFT );
	nk_label( sdlg_ctx, "Key", NK_TEXT_LEFT );
	nk_label( sdlg_ctx, "Alternate key", NK_TEXT_LEFT );
	sdlg_row( 330, 1, NULL );
	if ( nk_group_begin( sdlg_ctx, "keys", NK_WINDOW_BORDER ) )
	{
		for ( i = 0; i < s->nkeys; i++ )
		{
			sdlg_row( 22, 3, r );
			on = s->key_sel == i;
			if ( nk_selectable_label( sdlg_ctx, gui_key_control( i ), NK_TEXT_LEFT, &on ) )
			{
				s->key_sel = i;
				s->cap_slot = -1;
			}
			for ( slot = 0; slot < 2; slot++ )
			{
				int waiting = s->cap_slot == slot && s->cap_ctl == i;

				t = waiting ? "press a key..." : s->kw[i][slot] ? SDL_GetKeyName( s->kw[i][slot] ) : "-";
				on = waiting;
				if ( nk_selectable_label( sdlg_ctx, t, NK_TEXT_LEFT, &on ) )
				{
					s->key_sel = i;
					s->cap_ctl = i;
					s->cap_slot = waiting ? -1 : slot;
				}
			}
		}
		nk_group_end( sdlg_ctx );
	}
	sdlg_row( SDLG_ROW, 3, btn );
	if ( nk_button_label( sdlg_ctx, "Clear" ) && s->key_sel >= 0 )
	{
		s->kw[s->key_sel][0] = s->kw[s->key_sel][1] = 0;
		s->cap_slot = -1;
	}
	if ( nk_button_label( sdlg_ctx, "Restore defaults" ) )
	{
		for ( i = 0; i < s->nkeys; i++ )
		{
			s->kw[i][0] = gui_key_default( i, 0 );
			s->kw[i][1] = gui_key_default( i, 1 );
		}
		s->cap_slot = -1;
	}
	nk_spacing( sdlg_ctx, 1 );
	sdlg_text( "Click a key, then press the new one (click it again to leave it). A key given to" );
	sdlg_text( "one control leaves any other. Clear takes both keys off the selected control." );
	sdlg_text( "Two panel keys at once with the mouse: right-click holds a key down until clicked again." );
}

/**
  * @brief  The About page.
  *
  * @retval None.
  */
static void sdlg_page_about( void )
{
	char b[160];

	snprintf( b, sizeof b, DW_TITLE " emulator, version %s (built %s)", DW_VERSION, __DATE__ );
	sdlg_text( b );
	snprintf( b, sizeof b, "Firmware: %s", dw_fw_version[0] ? dw_fw_version : "(none loaded)" );
	sdlg_text( b );
	sdlg_row( 12, 1, NULL );
	sdlg_text( "An emulator of the E-mu Darwin 8-track digital audio disk recorder (1997), which runs the" );
	sdlg_text( "machine's original firmware. A non-commercial hobby project, made for preservation and" );
	sdlg_text( "nostalgia." );
	sdlg_row( 12, 1, NULL );
	sdlg_text( "This program is independent. It is not affiliated with, endorsed by or connected to" );
	sdlg_text( "E-mu Systems, or to Creative Technology Ltd, which owns the E-mu brand. E-mu, Darwin" );
	sdlg_text( "and the E-mu logo are trademarks of their owners and are used here only to name the" );
	sdlg_text( "hardware being emulated. No firmware is included: use the ROM images of your own unit." );
	sdlg_row( 12, 1, NULL );
	sdlg_text( "Built with Musashi (68000 core, Karl Stenerud), SDL2 (zlib licence) and Nuklear" );
	sdlg_text( "(public domain / MIT), with the Droid Sans font (Apache licence 2.0)." );
}

/**
  * @brief  Build the window: the tabs, the page (or the file browser), OK / Cancel.
  *
  * @param  w Window width.
  * @param  h Window height.
  *
  * @retval None.
  */
static void sdlg_ui( int w, int h )
{
	static const float ok[] = { 0.6f, 0.2f, 0.2f };
	nk_bool on;
	int i;

	if ( sdlg_br.active )
	{
		sdlg_br_ui( (float)h - 24 );
		return;
	}
	sdlg_row( 28, SDLG_PAGES, NULL );
	for ( i = 0; i < SDLG_PAGES; i++ )
	{
		on = sdlg_st.page == i;
		if ( nk_selectable_label( sdlg_ctx, sdlg_page_names[i], NK_TEXT_CENTERED, &on ) )
		{
			sdlg_st.page = i;
			sdlg_st.cap_slot = -1;
		}
	}
	sdlg_row( (float)h - 28 - 32 - 40, 1, NULL );
	if ( nk_group_begin( sdlg_ctx, "page", NK_WINDOW_BORDER ) )
	{
		switch ( sdlg_st.page )
		{
		case SDLG_AUDIO: sdlg_page_audio(); break;
		case SDLG_MIDI: sdlg_page_midi(); break;
		case SDLG_DISKS: sdlg_page_disks(); break;
		case SDLG_OPTIONS: sdlg_page_options(); break;
		case SDLG_KEYBOARD: sdlg_page_keys(); break;
		default: sdlg_page_about(); break;
		}
		nk_group_end( sdlg_ctx );
	}
	(void)w;
	sdlg_row( 30, 3, ok );
	nk_spacing( sdlg_ctx, 1 );
	if ( nk_button_label( sdlg_ctx, "OK" ) )
	{
		sdlg_ok = sdlg_done = 1;
	}
	if ( nk_button_label( sdlg_ctx, "Cancel" ) )
	{
		sdlg_done = 1;
	}
}

/**
  * @brief  Where disk images go when nothing says otherwise: the Disks folder beside the
  *         program (the release packages have one), else the program's own folder.
  *
  * @param  out Receives the folder, ending in a separator.
  * @param  n Room in out.
  *
  * @retval None.
  */
static void sdlg_disks_dir( char *out, size_t n )
{
	char dir[PLAT_PATH], disks[PLAT_PATH + 8];

	plat_exe_dir( dir, sizeof dir );
	snprintf( disks, sizeof disks, "%sDisks%c", dir, PLAT_SEP );
	if ( plat_is_dir( disks ) && snprintf( out, n, "%s", disks ) < (int)n )
	{
		return;
	}
	snprintf( out, n, "%s", dir );
}

/**
  * @brief  Windows' own file dialog (only on Windows).
  *
  * @param  title The dialog's title.
  * @param  path In: the current file; out: the file chosen.
  * @param  n Room in path.
  * @param  save 1 to name a new file.
  *
  * @retval 0 chosen, -1 cancelled, -2 not available here.
  */
static int sdlg_native_pick( const char *title, char *path, size_t n, int save )
{
#ifdef _WIN32
	char file[MAX_PATH], dir[PLAT_PATH];
	OPENFILENAMEA o;
	SDL_SysWMinfo info;
	const char *forced = getenv( "DARWIN_FILE_BROWSER" );

	if ( forced && forced[0] == '1' )
	{
		return -2;
	}
	snprintf( file, sizeof file, "%s", path );
	sdlg_disks_dir( dir, sizeof dir );
	memset( &o, 0, sizeof o );
	o.lStructSize = sizeof o;
	if ( !strchr( file, '\\' ) && !strchr( file, '/' ) )  // no folder given: start in Disks
	{
		o.lpstrInitialDir = dir;
	}
	SDL_VERSION( &info.version );
	if ( sdlg_win && SDL_GetWindowWMInfo( sdlg_win, &info ) )
	{
		o.hwndOwner = info.info.win.window;
	}
	o.lpstrFilter = "Disk images (*.img;*.hda;*.bin)\0*.img;*.hda;*.bin\0All files\0*.*\0";
	o.lpstrFile = file;
	o.nMaxFile = sizeof file;
	o.lpstrTitle = title;
	o.lpstrDefExt = save ? "img" : NULL;
	o.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | ( save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST );
	if ( !( save ? GetSaveFileNameA( &o ) : GetOpenFileNameA( &o ) ) )
	{
		return -1;
	}
	snprintf( path, n, "%s", file );
	return 0;
#else
	(void)title;
	(void)path;
	(void)n;
	(void)save;
	return -2;
#endif
}

/**
  * @brief  Sort the browser's entries: folders first, then by name without case.
  *
  * @param  a An entry.
  * @param  b Another.
  *
  * @retval <0, 0, >0 as strcmp.
  */
static int sdlg_entry_cmp( const void *a, const void *b )
{
	const sdlg_entry *x = a, *y = b;

	if ( x->dir != y->dir )
	{
		return y->dir - x->dir;
	}
	return plat_stricmp( x->name, y->name );
}

/**
  * @brief  plat_list_dir callback: add an entry to the browser's list.
  *
  * @param  name The entry.
  * @param  is_dir 1 for a folder.
  * @param  ctx Unused.
  *
  * @retval None.
  */
static void sdlg_br_add( const char *name, int is_dir, void *ctx )
{
	static const char *const ext[] = { ".img", ".hda", ".bin" };
	size_t len = strlen( name );
	sdlg_entry *e;
	int i, ok = is_dir || sdlg_br.all;

	(void)ctx;
	if ( name[0] == '.' || len >= sizeof e->name )  // hidden files, and names too long to keep
	{
		return;
	}
	for ( i = 0; !ok && i < 3; i++ )
	{
		ok = len > 4 && !plat_stricmp( name + len - 4, ext[i] );
	}
	if ( !ok )
	{
		return;
	}
	if ( sdlg_br.n == sdlg_br.cap )
	{
		e = realloc( sdlg_br.e, (size_t)( sdlg_br.cap ? sdlg_br.cap * 2 : 64 ) * sizeof *e );
		if ( !e )
		{
			return;
		}
		sdlg_br.e = e;
		sdlg_br.cap = sdlg_br.cap ? sdlg_br.cap * 2 : 64;
	}
	snprintf( sdlg_br.e[sdlg_br.n].name, sizeof sdlg_br.e[sdlg_br.n].name, "%s", name );
	sdlg_br.e[sdlg_br.n++].dir = is_dir;
}

/**
  * @brief  Read the browser's folder again.
  *
  * @retval None.
  */
static void sdlg_br_list( void )
{
	size_t len = strlen( sdlg_br.dir );

	if ( len && sdlg_br.dir[len - 1] != '/' && sdlg_br.dir[len - 1] != PLAT_SEP && len + 1 < sizeof sdlg_br.dir )
	{
		sdlg_br.dir[len] = PLAT_SEP;
		sdlg_br.dir[len + 1] = 0;
	}
	sdlg_br.n = 0;
	plat_list_dir( sdlg_br.dir, sdlg_br_add, NULL );
	qsort( sdlg_br.e, (size_t)sdlg_br.n, sizeof *sdlg_br.e, sdlg_entry_cmp );
}

/**
  * @brief  Open the file browser.
  *
  * @param  title What it asks for.
  * @param  path Where to start: a file (its folder) or a folder; "" the Disks folder (sdlg_disks_dir).
  * @param  save 1 to name a new file.
  * @param  target What the choice is for (sdlg_browser.target).
  *
  * @retval None.
  */
static void sdlg_br_start( const char *title, const char *path, int save, int target )
{
	char *s;

	sdlg_br.active = 1;
	sdlg_br.save = save;
	sdlg_br.target = target;
	sdlg_br.done = sdlg_br.ok = 0;
	sdlg_br.name[0] = 0;
	snprintf( sdlg_br.title, sizeof sdlg_br.title, "%s", title );
	if ( path[0] && plat_is_dir( path ) )
	{
		snprintf( sdlg_br.dir, sizeof sdlg_br.dir, "%s", path );
	}
	else if ( path[0] )
	{
		snprintf( sdlg_br.dir, sizeof sdlg_br.dir, "%s", path );
		s = strrchr( sdlg_br.dir, PLAT_SEP );
		if ( !s )
		{
			s = strrchr( sdlg_br.dir, '/' );
		}
		if ( s )
		{
			snprintf( sdlg_br.name, sizeof sdlg_br.name, "%s", s + 1 );
			s[1] = 0;
		}
		else
		{
			snprintf( sdlg_br.name, sizeof sdlg_br.name, "%s", sdlg_br.dir );
			sdlg_br.dir[0] = 0;
		}
		if ( !plat_is_dir( sdlg_br.dir ) )
		{
			sdlg_disks_dir( sdlg_br.dir, sizeof sdlg_br.dir );
		}
	}
	else
	{
		sdlg_disks_dir( sdlg_br.dir, sizeof sdlg_br.dir );
	}
	if ( save && !sdlg_br.name[0] )
	{
		snprintf( sdlg_br.name, sizeof sdlg_br.name, "darwin_%sMB.img", sdlg_st.newsize );
	}
	sdlg_br_list();
}

/**
  * @brief  The file browser: folder, list, file name, OK / Cancel.
  *
  * @param  h Room for it.
  *
  * @retval None.
  */
static void sdlg_br_ui( float h )
{
	static const float top[] = { 0.12f, 0.73f, 0.15f }, bottom[] = { 0.12f, 0.48f, 0.2f, 0.1f, 0.1f };
	char path[PLAT_PATH];
	nk_flags ev;
	nk_bool on;
	size_t len;
	int i;

	sdlg_text( sdlg_br.title );
	sdlg_row( SDLG_ROW, 3, top );
	nk_label( sdlg_ctx, "Folder:", NK_TEXT_LEFT );
	ev = nk_edit_string_zero_terminated( sdlg_ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, sdlg_br.dir,
										 (int)sizeof sdlg_br.dir, nk_filter_default );
	if ( ev & NK_EDIT_COMMITED )
	{
		sdlg_br_list();
	}
	if ( nk_button_label( sdlg_ctx, "Up" ) )
	{
		len = strlen( sdlg_br.dir );
		while ( len && ( sdlg_br.dir[len - 1] == PLAT_SEP || sdlg_br.dir[len - 1] == '/' ) )
		{
			sdlg_br.dir[--len] = 0;
		}
		while ( len && sdlg_br.dir[len - 1] != PLAT_SEP && sdlg_br.dir[len - 1] != '/' )
		{
			sdlg_br.dir[--len] = 0;
		}
		if ( !len )  // the top: "/" on Unix
		{
			snprintf( sdlg_br.dir, sizeof sdlg_br.dir, "%c", PLAT_SEP );
		}
		sdlg_br_list();
	}
	sdlg_row( h - 3 * SDLG_ROW - 20 - 60, 1, NULL );
	if ( nk_group_begin( sdlg_ctx, "files", NK_WINDOW_BORDER ) )
	{
		for ( i = 0; i < sdlg_br.n; i++ )
		{
			sdlg_row( 20, 1, NULL );
			snprintf( path, sizeof path, "%s%s", sdlg_br.e[i].name, sdlg_br.e[i].dir ? "/" : "" );
			on = !sdlg_br.e[i].dir && !strcmp( sdlg_br.e[i].name, sdlg_br.name );
			if ( nk_selectable_label( sdlg_ctx, path, NK_TEXT_LEFT, &on ) )
			{
				if ( sdlg_br.e[i].dir )
				{
					snprintf( path, sizeof path, "%s%s", sdlg_br.dir, sdlg_br.e[i].name );
					snprintf( sdlg_br.dir, sizeof sdlg_br.dir, "%s", path );
					sdlg_br_list();
					break;  // the list has changed under the loop
				}
				snprintf( sdlg_br.name, sizeof sdlg_br.name, "%s", sdlg_br.e[i].name );
			}
		}
		nk_group_end( sdlg_ctx );
	}
	sdlg_row( SDLG_ROW, 5, bottom );
	nk_label( sdlg_ctx, "File:", NK_TEXT_LEFT );
	nk_edit_string_zero_terminated( sdlg_ctx, NK_EDIT_FIELD, sdlg_br.name, (int)sizeof sdlg_br.name, nk_filter_default );
	on = sdlg_br.all;
	if ( nk_checkbox_label( sdlg_ctx, "All files", &on ) )
	{
		sdlg_br.all = on;
		sdlg_br_list();
	}
	if ( nk_button_label( sdlg_ctx, "OK" ) && sdlg_br.name[0] )
	{
		snprintf( path, sizeof path, "%s%s", sdlg_br.dir, sdlg_br.name );
		if ( !sdlg_br.save && !plat_exists( path ) )
		{
			sdlg_message( sdlg_br.title, "That file does not exist." );
		}
		else
		{
			sdlg_br.ok = sdlg_br.done = 1;
		}
	}
	if ( nk_button_label( sdlg_ctx, "Cancel" ) )
	{
		sdlg_br.done = 1;
	}
	if ( sdlg_br.done && sdlg_br.target >= 0 )  // inside the settings window: hand the choice on
	{
		sdlg_br_finish();
	}
}

/**
  * @brief  The browser has finished inside the settings window: use the choice.
  *
  * @retval None.
  */
static void sdlg_br_finish( void )
{
	char path[PLAT_PATH];
	int t = sdlg_br.target;

	sdlg_br.active = 0;
	if ( !sdlg_br.ok || snprintf( path, sizeof path, "%s%s", sdlg_br.dir, sdlg_br.name ) >= (int)sizeof path )
	{
		return;
	}
	if ( t < 100 )
	{
		snprintf( sdlg_st.disk[t], sizeof sdlg_st.disk[t], "%s", path );
		return;
	}
	if ( sd_create( path, (unsigned)atol( sdlg_st.newsize ) ) != 0 )
	{
		sdlg_message( "New disk image", "The image could not be created." );
		return;
	}
	snprintf( sdlg_st.disk[t - 100], sizeof sdlg_st.disk[t - 100], "%s", path );
	if ( atol( sdlg_st.newsize ) < 275 )
	{
		sdlg_message( "New disk image", "The Darwin does not format drives under 275 MB." );
	}
}

/**
  * @brief  "..." or "New..." on the Disks page: the system's file dialog, or the browser.
  *
  * @param  target The disk row (0-6), or 100 + row for a new image.
  * @param  save 1 to name a new file.
  * @param  title What is asked for.
  *
  * @retval None.
  */
static void sdlg_choose( int target, int save, const char *title )
{
	char path[PLAT_PATH];
	int row = target % 100, rc;

	if ( save )
	{
		snprintf( path, sizeof path, "darwin_%sMB.img", sdlg_st.newsize );
	}
	else
	{
		snprintf( path, sizeof path, "%s", sdlg_st.disk[row] );
	}
	rc = sdlg_native_pick( title, path, sizeof path, save );
	if ( rc == -2 )
	{
		sdlg_br_start( title, save ? "" : path, save, target );
		return;
	}
	if ( rc == 0 )
	{
		sdlg_br.ok = 1;
		sdlg_br.target = target;
		snprintf( sdlg_br.dir, sizeof sdlg_br.dir, "%s", "" );
		snprintf( sdlg_br.name, sizeof sdlg_br.name, "%s", "" );
		if ( strlen( path ) < sizeof sdlg_br.dir )
		{
			snprintf( sdlg_br.dir, sizeof sdlg_br.dir, "%s", path );  // the whole path; no name to add
		}
		sdlg_br_finish();
	}
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Show the settings window (modal) and save what the user accepts.
  *
  * @param  parent The panel window, may be NULL.
  * @param  redraw Draws the panel, may be NULL.
  *
  * @retval 1 accepted (settings saved), 0 cancelled.
  */
int sdlg_open( SDL_Window *parent, void ( *redraw )( void ) )
{
	char title[96];
	int w, h, shot_page;

	(void)parent;
	if ( !SDL_WasInit( SDL_INIT_AUDIO ) )
	{
		SDL_InitSubSystem( SDL_INIT_AUDIO );
	}
	if ( !SDL_WasInit( SDL_INIT_VIDEO ) )
	{
		SDL_InitSubSystem( SDL_INIT_VIDEO );
	}
	sdlg_load();
	if ( sdlg_shot )
	{
		sdlg_st.page = 0;
	}
	snprintf( title, sizeof title, "%s emulator settings", DW_TITLE );
	if ( sdlg_window_open( title, SDLG_W, SDLG_H ) != 0 )
	{
		dw_log( "settings window: %s\n", SDL_GetError() );
		return 0;
	}
	memset( &sdlg_br, 0, sizeof sdlg_br );
	sdlg_done = sdlg_ok = sdlg_quit = 0;
	sdlg_frames = 0;
	while ( !sdlg_done )
	{
		if ( sdlg_pump() )
		{
			break;  // closed: as Cancel
		}
		SDL_GetWindowSize( sdlg_win, &w, &h );
		if ( nk_begin( sdlg_ctx, "settings", nk_rect( 0, 0, (float)w, (float)h ), NK_WINDOW_NO_SCROLLBAR ) )
		{
			sdlg_ui( w, h );
		}
		nk_end( sdlg_ctx );
		shot_page = -1;
		if ( sdlg_shot && ++sdlg_frames % 4 == 0 )  // -dlgshot: each page once it has settled
		{
			shot_page = sdlg_br.active ? SDLG_PAGES : sdlg_st.page;
		}
		sdlg_render( shot_page );
		if ( shot_page >= 0 )
		{
			if ( sdlg_br.active )  // the file browser, after the pages
			{
				sdlg_done = 1;
			}
			else if ( ++sdlg_st.page == SDLG_PAGES )
			{
				sdlg_st.page = SDLG_DISKS;
				sdlg_br_start( "Disk image", "", 0, 0 );
				sdlg_frames = 0;
			}
		}
		if ( redraw )
		{
			redraw();
		}
		SDL_Delay( 15 );
	}
	sdlg_last_page = sdlg_shot ? sdlg_last_page : sdlg_st.page;
	free( sdlg_br.e );
	memset( &sdlg_br, 0, sizeof sdlg_br );
	if ( sdlg_ok )
	{
		sdlg_accept();
	}
	sdlg_window_close();
	if ( parent )
	{
		SDL_RaiseWindow( parent );
	}
	if ( sdlg_quit )
	{
		SDL_Event q;

		memset( &q, 0, sizeof q );
		q.type = SDL_QUIT;
		SDL_PushEvent( &q );
	}
	if ( sdlg_ok && sdlg_resize )
	{
		gui_resize();
		sdlg_resize = 0;
	}
	return sdlg_ok;
}

/**
  * @brief  Make the next sdlg_open capture each page to PREFIXn.bmp and close (-dlgshot).
  *
  * @param  prefix File name prefix.
  *
  * @retval None.
  */
void sdlg_snapshot( const char *prefix )
{
	sdlg_shot = prefix;
}

/**
  * @brief  Ask for a disk image file.
  *
  * @param  parent The panel window, may be NULL.
  * @param  redraw Draws the panel meanwhile, may be NULL.
  * @param  title The window's title.
  * @param  path In: where to start; out: the file chosen.
  * @param  n Room in path.
  * @param  save 1 to name a new file, 0 to pick an existing one.
  *
  * @retval 0 chosen, -1 cancelled.
  */
int sdlg_pick_file( SDL_Window *parent, void ( *redraw )( void ), const char *title, char *path, size_t n, int save )
{
	char out[PLAT_PATH];
	int rc = sdlg_native_pick( title, path, n, save ), w, h;

	if ( rc != -2 )
	{
		return rc;
	}
	memset( &sdlg_st, 0, sizeof sdlg_st );
	snprintf( sdlg_st.newsize, sizeof sdlg_st.newsize, "1000" );
	sdlg_st.cap_slot = -1;
	if ( sdlg_window_open( title, 640, 480 ) != 0 )
	{
		return -1;
	}
	memset( &sdlg_br, 0, sizeof sdlg_br );
	sdlg_br_start( title, path, save, -1 );
	sdlg_quit = 0;
	while ( !sdlg_br.done )
	{
		if ( sdlg_pump() )
		{
			break;
		}
		SDL_GetWindowSize( sdlg_win, &w, &h );
		if ( nk_begin( sdlg_ctx, "files", nk_rect( 0, 0, (float)w, (float)h ), NK_WINDOW_NO_SCROLLBAR ) )
		{
			sdlg_br_ui( (float)h - 24 );
		}
		nk_end( sdlg_ctx );
		sdlg_render( -1 );
		if ( redraw )
		{
			redraw();
		}
		SDL_Delay( 15 );
	}
	rc = sdlg_br.ok && snprintf( out, sizeof out, "%s%s", sdlg_br.dir, sdlg_br.name ) < (int)sizeof out &&
		 strlen( out ) < n ? 0 : -1;
	if ( rc == 0 )
	{
		snprintf( path, n, "%s", out );
	}
	free( sdlg_br.e );
	memset( &sdlg_br, 0, sizeof sdlg_br );
	sdlg_window_close();
	if ( parent )
	{
		SDL_RaiseWindow( parent );
	}
	if ( sdlg_quit )
	{
		SDL_Event q;

		memset( &q, 0, sizeof q );
		q.type = SDL_QUIT;
		SDL_PushEvent( &q );
	}
	return rc;
}
