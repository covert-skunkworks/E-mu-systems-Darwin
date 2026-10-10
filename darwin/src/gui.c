//  Description:
//
//		gui.c -- SDL2 front end: the Darwin's front panel in a window.
//
//		The panel is drawn in a fixed panel
//		space (GUI_PW x GUI_PH units) into a render-target texture and
//		scaled to the window, an element table for the keys with their PC
//		shortcuts, the live LCD, LEDs and the data wheel.
//
//		The artwork is UI.svg (297 x 73 mm, drawn after the real panel),
//		rendered once by svg.c into a background texture at GUI_S panel
//		units per millimetre. Only what changes is drawn over it: the LCD,
//		lit meter segments and LEDs, armed track keys, pressed keys and a
//		mark on the data wheel. All positions below are in the artwork's
//		millimetres. The artwork is looked
//		for next to the program, then in the folder above it (the project
//		root, so an edited file shows without a rebuild); the copy linked
//		into the program is the fallback.
//
//		A mouse presses one key at a time, but the Darwin wants two at once
//		for some things (record: hold REC, press PLAY). A right-click holds a
//		key down until it is clicked again.
//
//		Every key is a (group, index) pair of the panel link (panel.h):
//
//		  group 0  DEC, INC, STO LOC, GOTO      group 4  PLAY STOP REC REW F.FWD RTZ
//		  group 1  cursor: left, right, down, up group 5  track keys 1-8
//		  group 2  keypad 0-9                   group 6  Input Mode, Rehearse,
//		  group 3  F1-F6, EXIT                           assignable 1, 2
//
//		The GUI thread takes the emulator lock around everything that touches
//		emulator state (keys into the link, reading the display).
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "gui.h"
#include "darwin.h"
#include "panel.h"
#include "plat.h"
#include "scsidisk.h"
#include "audio.h"
#include "setdlg.h"
#include "sim68306.h"
#include "wd33c93.h"
#include "settings.h"
#include "svg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <SDL2/SDL.h>
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>
#else
#include "../res/icon_rgba.h"
#endif

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define GUI_S  10.0f  // panel units per millimetre of the artwork
#define GUI_PW 2970   // panel space: the artwork's 297 x 73.025 mm
#define GUI_PH 730

// the LCD window of the artwork, and the 240 x 64 dots centred in it
#define GUI_LCD_X0 107.586f
#define GUI_LCD_Y0 8.843f
#define GUI_LCD_X1 189.398f
#define GUI_LCD_Y1 33.013f
#define GUI_LCD_DOT 0.33f  // mm per dot

// meters: column t, segment s (0 = top, 0 dB)
#define GUI_MTR_X  40.886f
#define GUI_MTR_DX 7.1155f
#define GUI_MTR_W  2.754f
#define GUI_MTR_Y  17.225f
#define GUI_MTR_DY 1.6416f
#define GUI_MTR_H  0.918f

#define GUI_WHEEL_X 227.82f
#define GUI_WHEEL_Y 18.12f
#define GUI_WHEEL_R 12.47f
#define GUI_DIMPLE_D 6.6f  // dimple centre from the wheel centre, mm
#define GUI_DIMPLE_R 4.2f  // dimple radius, mm

#define GUI_POWER_X 22.89f
#define GUI_POWER_Y 29.81f
#define GUI_POWER_R 5.6f

#define GUI_VOL_X    22.96f  // the headphone VOLUME knob (an analog pot: audio.c au_set_phones)
#define GUI_VOL_Y    47.14f
#define GUI_VOL_R    3.8f
#define GUI_VOL_DRAG 25.0f   // mm of vertical drag for the whole travel
#define GUI_VOL_ARC  150.0f  // degrees either side of straight up

#define GUI_LED_MIDI_X 39.10f  // the dots by INPUT MODE and REHEARSE: MIDI and SCSI activity
#define GUI_LED_MIDI_Y 61.15f
#define GUI_LED_SCSI_X 52.30f
#define GUI_LED_SCSI_Y 61.43f

#define GUI_LED_R 0.85f  // radius of a lit LED lens (the artwork's dots are 0.71)

#define GUI_WHEEL_STEP 0.21f  // radians of drag per wheel step
#define GUI_PI         3.14159265f

enum { GUI_ROUND = 0, GUI_RECT };

//------------------------------------------------------------------------------
//---------------------- Local types and structures ----------------------------
//------------------------------------------------------------------------------

typedef struct
{
	uint8_t r, g, b;
} gui_rgb;

typedef struct
{
	const char *label;      // for the reader: what the key is
	int         kind;
	float       x, y;       // centre, mm
	float       w, h;       // size, mm
	int         group, index;
	int         led_group, led_index;  // -1 = no LED; the track keys light themselves
	float       lx, ly;     // LED centre, mm
	SDL_Keycode key, key2;
	int         down;
	int         held;       // right-clicked: stays down until clicked again (e.g. REC while PLAY is clicked)
} gui_elem;

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

// the copy of UI.svg built into the program
extern const unsigned char ui_svg[];  // tools/bin2c.c writes it to build/gen_ui_svg.c
extern const unsigned long ui_svg_len;

#define TRK( n, k ) { #n, GUI_RECT, 42.263f + ( n - 1 ) * 7.1155f, 46.347f, 3.204f, 5.44f, 5, n - 1, 5, n - 1, 0, 0, k, 0, 0, 0 }
#define FK( l, i, x, k ) { l, GUI_ROUND, x, 44.93f, 7.1f, 5.84f, 3, i, -1, 0, 0, 0, k, 0, 0, 0 }
#define TP( l, i, lx, k ) { l, GUI_RECT, lx + 2.83f, 61.37f, 9.64f, 8.75f, 4, i, 4, i, lx, 55.54f, k, 0, 0, 0 }
#define PAD( l, i, x, y, k, k2 ) { l, GUI_ROUND, x, y, 7.1f, 5.84f, 2, i, -1, 0, 0, 0, k, k2, 0, 0 }

static gui_elem gui_el[] = {
	TRK( 1, SDLK_q ), TRK( 2, SDLK_w ), TRK( 3, SDLK_e ), TRK( 4, SDLK_r ),
	TRK( 5, SDLK_t ), TRK( 6, SDLK_y ), TRK( 7, SDLK_u ), TRK( 8, SDLK_i ),
	FK( "EXIT", 6, 104.65f, SDLK_ESCAPE ),
	FK( "F1", 0, 117.23f, SDLK_F1 ), FK( "F2", 1, 129.97f, SDLK_F2 ), FK( "F3", 2, 143.22f, SDLK_F3 ),
	FK( "F4", 3, 156.14f, SDLK_F4 ), FK( "F5", 4, 169.37f, SDLK_F5 ), FK( "F6", 5, 182.12f, SDLK_F6 ),
	{ "INPUT MODE", GUI_ROUND, 44.40f, 63.51f, 7.1f, 5.84f, 6, 0, 6, 0, 39.10f, 61.15f, SDLK_F7, 0, 0, 0 },
	{ "REHEARSE", GUI_ROUND, 57.60f, 63.78f, 7.1f, 5.84f, 6, 1, 6, 1, 52.30f, 61.43f, SDLK_F8, 0, 0, 0 },
	{ "USER 1", GUI_ROUND, 70.80f, 64.06f, 7.1f, 5.84f, 6, 2, 6, 2, 65.50f, 61.70f, SDLK_F9, 0, 0, 0 },
	{ "USER 2", GUI_ROUND, 83.59f, 64.06f, 7.1f, 5.84f, 6, 3, 6, 3, 78.29f, 61.70f, SDLK_F10, 0, 0, 0 },
	// link indices, from the firmware (keys and LEDs alike): 0 PLAY, 1 STOP, 2 REC, 3 REW, 4 F.FWD, 5 RTZ
	TP( "RTZ", 5, 106.99f, SDLK_HOME ), TP( "REW", 3, 120.31f, SDLK_PAGEUP ), TP( "F.FWD", 4, 133.66f, SDLK_PAGEDOWN ),
	TP( "STOP", 1, 146.33f, SDLK_END ), TP( "PLAY", 0, 159.79f, SDLK_SPACE ), TP( "REC", 2, 172.82f, SDLK_F12 ),
	{ "UP", GUI_ROUND, 196.28f, 48.16f, 10.15f, 7.93f, 1, 2, -1, 0, 0, 0, SDLK_UP, 0, 0, 0 },
	{ "DOWN", GUI_ROUND, 196.26f, 63.54f, 10.15f, 7.93f, 1, 3, -1, 0, 0, 0, SDLK_DOWN, 0, 0, 0 },
	{ "LEFT", GUI_ROUND, 188.54f, 55.82f, 7.93f, 10.15f, 1, 0, -1, 0, 0, 0, SDLK_LEFT, 0, 0, 0 },
	{ "RIGHT", GUI_ROUND, 203.97f, 55.85f, 7.93f, 10.15f, 1, 1, -1, 0, 0, 0, SDLK_RIGHT, 0, 0, 0 },
	{ "DEC", GUI_ROUND, 212.80f, 33.75f, 7.1f, 5.84f, 0, 0, -1, 0, 0, 0, SDLK_LEFTBRACKET, SDLK_KP_MINUS, 0, 0 },
	{ "INC", GUI_ROUND, 246.99f, 33.75f, 7.1f, 5.84f, 0, 1, -1, 0, 0, 0, SDLK_RIGHTBRACKET, SDLK_KP_PLUS, 0, 0 },
	PAD( "1", 1, 216.62f, 40.98f, SDLK_1, SDLK_KP_1 ), PAD( "2", 2, 229.31f, 40.98f, SDLK_2, SDLK_KP_2 ),
	PAD( "3", 3, 241.82f, 40.98f, SDLK_3, SDLK_KP_3 ), PAD( "4", 4, 216.62f, 49.54f, SDLK_4, SDLK_KP_4 ),
	PAD( "5", 5, 229.31f, 49.54f, SDLK_5, SDLK_KP_5 ), PAD( "6", 6, 241.82f, 49.54f, SDLK_6, SDLK_KP_6 ),
	PAD( "7", 7, 216.62f, 58.26f, SDLK_7, SDLK_KP_7 ), PAD( "8", 8, 229.31f, 58.26f, SDLK_8, SDLK_KP_8 ),
	PAD( "9", 9, 241.82f, 58.26f, SDLK_9, SDLK_KP_9 ), PAD( "0", 0, 229.31f, 66.80f, SDLK_0, SDLK_KP_0 ),
	{ "STO LOC", GUI_ROUND, 216.62f, 66.80f, 7.1f, 5.84f, 0, 2, -1, 0, 0, 0, SDLK_INSERT, 0, 0, 0 },
	{ "GOTO", GUI_ROUND, 241.82f, 66.80f, 7.1f, 5.84f, 0, 3, -1, 0, 0, 0, SDLK_RETURN, SDLK_KP_ENTER, 0, 0 },
};

#undef TRK
#undef FK
#undef TP
#undef PAD

#define GUI_NEL ( (int)( sizeof gui_el / sizeof gui_el[0] ) )

// the controls with keyboard keys: the table's (its key / key2 are the defaults), then the data
// wheel left and right
#define GUI_NKEYS ( GUI_NEL + 2 )

static SDL_Keycode gui_keys[GUI_NKEYS][2];      // the keys in use (gui_keys_load)
static char        gui_key_names[GUI_NKEYS][24];  // "PLAY", "Track 1", "Keypad 1" ...

static const SDL_FRect gui_bay = { 256.43f, 7.12f, 25.22f, 64.08f };  // drive bay (mm): the disk image
static const SDL_FRect gui_logo = { 18.0f, 7.0f, 10.0f, 12.0f };     // logo (mm): the settings window

static SDL_Window   *gui_win;
static SDL_Renderer *gui_ren;
static SDL_Texture  *gui_frame_tex, *gui_lcd_tex, *gui_bg_tex;
static SDL_Rect      gui_view;
static float         gui_wheel_angle;
static uint8_t       gui_lcd[PNL_ROWB * PNL_H];
static uint8_t       gui_leds[8][16];
static uint8_t       gui_meter[8];
static int           gui_contrast = 8;
static unsigned      gui_midi_count, gui_midi_seen, gui_scsi_count, gui_scsi_seen;
static int           gui_midi_level;  // the MIDI LED's pin is low (lit) right now

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void gui_color( gui_rgb c, uint8_t a );
static void gui_rect( float x, float y, float w, float h, gui_rgb c, uint8_t a );
static void gui_ellipse( float cx, float cy, float rx, float ry, gui_rgb c, uint8_t a );
static int gui_load_art( const char *dir );
static void gui_set_icon( void );
static void gui_window_size( int display, int *w, int *h );
static int gui_led_lit( int v );
static void gui_draw_led( float x, float y, gui_rgb c );
static void gui_draw_activity( void );
static void gui_draw_elem( const gui_elem *e );
static void gui_draw_dimple( float x, float y );
static void gui_line( float x0, float y0, float x1, float y1, float w, gui_rgb c );
static void gui_draw_volume( void );
static void gui_set_volume( int v );
static void gui_draw_meters( void );
static void gui_draw_lcd( void );
static void gui_draw( void );
static gui_elem *gui_hit( float x, float y );
static void gui_press( gui_elem *e, int down );
static void gui_to_panel( int wx, int wy, float *x, float *y );
static void gui_choose_disk( void );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Set the draw colour.
  *
  * @param  c Colour.
  * @param  a Alpha.
  *
  * @retval None.
  */
static void gui_color( gui_rgb c, uint8_t a ) { SDL_SetRenderDrawColor( gui_ren, c.r, c.g, c.b, a ); }

/**
  * @brief  Fill a rectangle given in millimetres.
  *
  * @param  x Left.
  * @param  y Top.
  * @param  w Width.
  * @param  h Height.
  * @param  c Colour.
  * @param  a Alpha.
  *
  * @retval None.
  */
static void gui_rect( float x, float y, float w, float h, gui_rgb c, uint8_t a )
{
	SDL_FRect r = { x * GUI_S, y * GUI_S, w * GUI_S, h * GUI_S };

	gui_color( c, a );
	SDL_RenderFillRectF( gui_ren, &r );
}

/**
  * @brief  Fill an ellipse given in millimetres, as horizontal spans.
  *
  * @param  cx Centre x.
  * @param  cy Centre y.
  * @param  rx Radius x.
  * @param  ry Radius y.
  * @param  c Colour.
  * @param  a Alpha.
  *
  * @retval None.
  */
static void gui_ellipse( float cx, float cy, float rx, float ry, gui_rgb c, uint8_t a )
{
	float y, hw;
	SDL_FRect r;

	cx *= GUI_S;
	cy *= GUI_S;
	rx *= GUI_S;
	ry *= GUI_S;
	gui_color( c, a );
	for ( y = -ry + 0.5f; y < ry; y += 1.0f )
	{
		hw = rx * sqrtf( 1 - ( y * y ) / ( ry * ry ) );
		r.x = cx - hw;
		r.y = cy + y - 0.5f;
		r.w = 2 * hw;
		r.h = 1.0f;
		SDL_RenderFillRectF( gui_ren, &r );
	}
}

/**
  * @brief  The window's size: 90 % of the display's width (at most the panel's own 2970), or half that.
  *
  * @param  display The display the window is on.
  * @param  w Receives the width.
  * @param  h Receives the height (the panel's proportions).
  *
  * @retval None.
  */
static void gui_window_size( int display, int *w, int *h )
{
	SDL_Rect ub = { 0, 0, 1600, 900 };

	SDL_GetDisplayUsableBounds( display, &ub );
	*w = ub.w * 90 / 100;
	*w = *w > GUI_PW ? GUI_PW : *w;
	*w = set_cfg.ui_half ? *w / 2 : *w;  // settings: half size
	*h = *w * GUI_PH / GUI_PW;
}

/**
  * @brief  Give the window the program's icon: title bar and taskbar.
  *
  * Windows takes resource 1 (res/darwin.rc), which has every size; elsewhere
  * SDL gets the 64 x 64 image (res/icon_rgba.h).
  *
  * @retval None.
  */
static void gui_set_icon( void )
{
#ifndef _WIN32
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom( (void *)icon_rgba, 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32 );

	if ( s )
	{
		SDL_SetWindowIcon( gui_win, s );
		SDL_FreeSurface( s );
	}
#else
	SDL_SysWMinfo info;
	HINSTANCE inst = GetModuleHandleA( NULL );
	HANDLE big, small;

	SDL_VERSION( &info.version );
	if ( !SDL_GetWindowWMInfo( gui_win, &info ) )
	{
		return;
	}
	big = LoadImageA( inst, MAKEINTRESOURCEA( 1 ), IMAGE_ICON, GetSystemMetrics( SM_CXICON ),
					  GetSystemMetrics( SM_CYICON ), 0 );
	small = LoadImageA( inst, MAKEINTRESOURCEA( 1 ), IMAGE_ICON, GetSystemMetrics( SM_CXSMICON ),
						GetSystemMetrics( SM_CYSMICON ), 0 );
	if ( big )
	{
		SendMessageA( info.info.win.window, WM_SETICON, ICON_BIG, (LPARAM)big );
	}
	if ( small )
	{
		SendMessageA( info.info.win.window, WM_SETICON, ICON_SMALL, (LPARAM)small );
	}
#endif
}

/**
  * @brief  Render the panel artwork into the background texture.
  *
  * @param  dir The program's folder, with a trailing backslash.
  *
  * @retval 0 drawn, -1 no usable artwork.
  */
static int gui_load_art( const char *dir )
{
	static const char *const where[] = { "%sUI.svg", "%s../UI.svg" };  // '/' works on Windows too
	char path[PLAT_PATH];
	char *doc = NULL;
	const char *art = (const char *)ui_svg;
	size_t len = (size_t)ui_svg_len;
	uint32_t *px;
	FILE *f;
	long n;
	int i, rc;

	for ( i = 0; i < 2 && !doc; i++ )
	{
		snprintf( path, sizeof path, where[i], dir );
		if ( !( f = fopen( path, "rb" ) ) )
		{
			continue;
		}
		fseek( f, 0, SEEK_END );
		n = ftell( f );
		fseek( f, 0, SEEK_SET );
		if ( n > 0 && ( doc = malloc( (size_t)n ) ) && fread( doc, 1, (size_t)n, f ) == (size_t)n )
		{
			art = doc;
			len = (size_t)n;
		}
		else
		{
			free( doc );
			doc = NULL;
		}
		fclose( f );
	}
	px = malloc( (size_t)GUI_PW * GUI_PH * sizeof *px );
	if ( !px )
	{
		free( doc );
		return -1;
	}
	for ( i = 0; i < GUI_PW * GUI_PH; i++ )
	{
		px[i] = 0xFF2C2D30;
	}
	rc = svg_render( art, len, px, GUI_PW, GUI_PH );
	free( doc );
	if ( rc == 0 )
	{
		SDL_UpdateTexture( gui_bg_tex, NULL, px, GUI_PW * (int)sizeof *px );
	}
	free( px );
	return rc == 0 ? 0 : -1;
}

/**
  * @brief  Is an LED lit at this moment?
  *
  * The panel link sends 1 for on and larger values for flashing (4 for an
  * armed track, 2 and 5 while recording starts); the rates are a guess:
  * even values flash at 2 Hz, odd ones at 4 Hz.
  *
  * @param  v The LED's state.
  *
  * @retval 1 lit now, 0 dark.
  */
static int gui_led_lit( int v )
{
	Uint32 ms = SDL_GetTicks();

	if ( v <= 1 )
	{
		return v;
	}
	return v & 1 ? ( ms / 125 ) & 1 : ( ms / 250 ) & 1;
}

/**
  * @brief  A lit LED over one of the artwork's LED dots.
  *
  * @param  x Centre x, mm.
  * @param  y Centre y, mm.
  * @param  c Colour.
  *
  * @retval None.
  */
static void gui_draw_led( float x, float y, gui_rgb c )
{
	// a glow wider than the lens, then the lens itself (larger than the artwork's dot) and a hot centre
	gui_ellipse( x, y, GUI_LED_R * 2.8f, GUI_LED_R * 2.8f, c, 0x30 );
	gui_ellipse( x, y, GUI_LED_R * 1.9f, GUI_LED_R * 1.9f, c, 0x60 );
	gui_ellipse( x, y, GUI_LED_R, GUI_LED_R, c, 0xFF );
	gui_ellipse( x - 0.15f, y - 0.15f, GUI_LED_R * 0.4f, GUI_LED_R * 0.4f, ( gui_rgb ){ 0xFF, 0xF0, 0xD0 }, 0xC0 );
}

/**
  * @brief  The MIDI and SCSI activity LEDs (the dots by INPUT MODE and REHEARSE).
  *
  * The real panel prints "MIDI" and "SCSI" under these two LEDs, the only
  * ones by the INPUT MODE and REHEARSE keys, so they also show those keys'
  * states (panel LEDs 6.0 / 6.1). MIDI activity is the 68306's port A
  * bit 1 (active low), which the firmware pulses for 5 ms per byte
  * received (ROM 04D80C); SCSI is bus activity. Both are counted in the
  * emulator and held for 60 ms here, so short pulses show.
  *
  * @retval None.
  */
static void gui_draw_activity( void )
{
	static const gui_rgb amber = { 0xFF, 0xB0, 0x20 };
	static Uint32 midi_until, scsi_until;
	Uint32 now = SDL_GetTicks();

	if ( gui_midi_count != gui_midi_seen || gui_midi_level )
	{
		gui_midi_seen = gui_midi_count;
		midi_until = now + 60;
	}
	if ( gui_scsi_count != gui_scsi_seen )
	{
		gui_scsi_seen = gui_scsi_count;
		scsi_until = now + 60;
	}
	// each LED: the firmware's state for the key beside it (Input Mode 6.0, Rehearse 6.1), or activity
	if ( dw_powered && ( (Sint32)( midi_until - now ) > 0 || gui_led_lit( gui_leds[6][0] ) ) )
	{
		gui_draw_led( GUI_LED_MIDI_X, GUI_LED_MIDI_Y, amber );
	}
	if ( dw_powered && ( (Sint32)( scsi_until - now ) > 0 || gui_led_lit( gui_leds[6][1] ) ) )
	{
		gui_draw_led( GUI_LED_SCSI_X, GUI_LED_SCSI_Y, amber );
	}
}

/**
  * @brief  Draw one key's live state: its LED, and a shade while pressed.
  *
  * @param  e The key.
  *
  * @retval None.
  */
static void gui_draw_elem( const gui_elem *e )
{
	static const gui_rgb led_on = { 0xFF, 0x30, 0x18 }, amber = { 0xFF, 0xB0, 0x20 }, shade = { 0, 0, 0 };
	int lit = e->led_group >= 0 && gui_led_lit( gui_leds[e->led_group & 7][e->led_index & 15] );

	gui_rgb c = e->led_group == 4 && e->index == 2 ? led_on : amber;  // REC red, the others amber

	if ( e->group == 5 )  // track key: the cap lights red while armed, with a glow around it
	{
		if ( lit )
		{
			gui_rect( e->x - e->w / 2 - 0.6f, e->y - e->h / 2 - 0.6f, e->w + 1.2f, e->h + 1.2f, led_on, 0x50 );
			gui_rect( e->x - e->w / 2 + 0.2f, e->y - e->h / 2 + 0.2f, e->w - 0.4f, e->h - 0.4f, led_on, 0xF0 );
		}
	}
	else if ( e->group == 6 )
	{
		// INPUT MODE / REHEARSE light the panel's two LEDs (printed MIDI and SCSI), which also show
		// MIDI and SCSI activity: gui_draw_activity draws them. The assignable keys have no LED.
	}
	else if ( e->led_group >= 0 && lit )
	{
		gui_draw_led( e->lx, e->ly, c );
	}
	if ( e->down )
	{
		if ( e->kind == GUI_ROUND )
		{
			gui_ellipse( e->x, e->y, e->w / 2, e->h / 2, shade, 0x60 );
		}
		else
		{
			gui_rect( e->x - e->w / 2, e->y - e->h / 2, e->w, e->h, shade, 0x60 );
		}
	}
}

/**
  * @brief  The data wheel's finger dimple: a dished circle lit from the upper left.
  *
  * The shading runs from 4C to EB grey like the dimple the artwork used
  * to carry, in finer steps so it shows no bands at this size.
  *
  * @param  x Centre x, mm.
  * @param  y Centre y, mm.
  *
  * @retval None.
  */
static void gui_draw_dimple( float x, float y )
{
	float r = GUI_DIMPLE_R, k;
	uint8_t g;
	int i;

	gui_ellipse( x, y, r + 0.08f, r + 0.08f, ( gui_rgb ){ 0, 0, 0 }, 0xFF );
	for ( i = 0; i < 28; i++ )
	{
		k = (float)i / 28;
		g = (uint8_t)( 0x4C + ( 0xEB - 0x4C ) * k );
		// each lighter disc smaller and nearer the light (upper left)
		gui_ellipse( x - k * r * 0.32f, y - k * r * 0.42f, r * ( 1 - k * 0.92f ), r * ( 1 - k * 0.92f ),
					 ( gui_rgb ){ g, g, g }, 0xFF );
	}
}

/**
  * @brief  A straight line of some width, in millimetres.
  *
  * @param  x0 From x.
  * @param  y0 From y.
  * @param  x1 To x.
  * @param  y1 To y.
  * @param  w Width.
  * @param  c Colour.
  *
  * @retval None.
  */
static void gui_line( float x0, float y0, float x1, float y1, float w, gui_rgb c )
{
	static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
	SDL_Vertex v[4];
	float dx = x1 - x0, dy = y1 - y0, l = hypotf( dx, dy ), nx, ny;
	int i;

	if ( l <= 0 )
	{
		return;
	}
	nx = -dy / l * w / 2;
	ny = dx / l * w / 2;
	v[0].position = ( SDL_FPoint ){ ( x0 + nx ) * GUI_S, ( y0 + ny ) * GUI_S };
	v[1].position = ( SDL_FPoint ){ ( x1 + nx ) * GUI_S, ( y1 + ny ) * GUI_S };
	v[2].position = ( SDL_FPoint ){ ( x1 - nx ) * GUI_S, ( y1 - ny ) * GUI_S };
	v[3].position = ( SDL_FPoint ){ ( x0 - nx ) * GUI_S, ( y0 - ny ) * GUI_S };
	for ( i = 0; i < 4; i++ )
	{
		v[i].color = ( SDL_Color ){ c.r, c.g, c.b, 0xFF };
		v[i].tex_coord = ( SDL_FPoint ){ 0, 0 };
	}
	SDL_RenderGeometry( gui_ren, NULL, v, 4, idx, 6 );
}

/**
  * @brief  The VOLUME knob: its face drawn again over the artwork's, with the pointer where it is turned.
  *
  * @retval None.
  */
static void gui_draw_volume( void )
{
	float a = ( -GUI_VOL_ARC + 2 * GUI_VOL_ARC * set_cfg.phones_volume / 100.0f ) * GUI_PI / 180;

	gui_ellipse( GUI_VOL_X, GUI_VOL_Y, 3.08f, 3.08f, ( gui_rgb ){ 0x19, 0x19, 0x19 }, 0xFF );
	gui_ellipse( GUI_VOL_X, GUI_VOL_Y, 2.59f, 2.59f, ( gui_rgb ){ 0, 0, 0 }, 0xFF );
	// 0 = straight up, clockwise louder: the artwork's pointer runs from 0.4 to 2.4 mm out
	gui_line( GUI_VOL_X + 0.4f * sinf( a ), GUI_VOL_Y - 0.4f * cosf( a ), GUI_VOL_X + 2.4f * sinf( a ),
			  GUI_VOL_Y - 2.4f * cosf( a ), 0.3f, ( gui_rgb ){ 0xFF, 0xFF, 0xFF } );
}

/**
  * @brief  Turn the VOLUME knob.
  *
  * @param  v Position 0-100 (clamped).
  *
  * @retval None.
  */
static void gui_set_volume( int v )
{
	set_cfg.phones_volume = v < 0 ? 0 : v > 100 ? 100 : v;
	au_set_phones( set_cfg.phones_volume / 100.0f );
}

/**
  * @brief  Light the meter segments (10 per track, from the bottom).
  *
  * @retval None.
  */
static void gui_draw_meters( void )
{
	static const gui_rgb on = { 0xFF, 0xB0, 0x20 }, peak = { 0xFF, 0x40, 0x20 };
	int t, s, n;

	for ( t = 0; t < 8; t++ )
	{
		n = gui_meter[t] / 12;  // the firmware sends 0-127, 12 per segment (ROM table 00A7E0)
		n = n > 10 ? 10 : n;
		for ( s = 10 - n; s < 10; s++ )
		{
			gui_rect( GUI_MTR_X + t * GUI_MTR_DX, GUI_MTR_Y + s * GUI_MTR_DY, GUI_MTR_W, GUI_MTR_H, s ? on : peak, 0xFF );
		}
	}
}

/**
  * @brief  Draw the LCD: the panel's 240 x 64 bitmap on blue-grey STN glass.
  *
  * @retval None.
  */
static void gui_draw_lcd( void )
{
	float dw = PNL_W * GUI_LCD_DOT, dh = PNL_H * GUI_LCD_DOT;
	float x0 = ( GUI_LCD_X0 + GUI_LCD_X1 - dw ) / 2, y0 = ( GUI_LCD_Y0 + GUI_LCD_Y1 - dh ) / 2;
	SDL_FRect dst = { x0 * GUI_S, y0 * GUI_S, dw * GUI_S, dh * GUI_S };
	uint32_t bg, dot, gap;
	void *tp;
	int pitch, x, y, lit;
	float t;

	if ( !dw_powered )
	{
		gui_rect( GUI_LCD_X0, GUI_LCD_Y0, GUI_LCD_X1 - GUI_LCD_X0, GUI_LCD_Y1 - GUI_LCD_Y0, ( gui_rgb ){ 0x7E, 0x88, 0x8C },
				  0xFF );
		return;
	}
	// contrast 0-15: low values wash the dots out, as on the real glass
	t = gui_contrast >= 6 ? 1.0f : gui_contrast / 6.0f;
	bg = 0xFFA8BCC6;
	dot = 0xFF000000u | ( (uint32_t)( 0xA8 - ( 0xA8 - 0x18 ) * t ) << 16 ) |
		  ( (uint32_t)( 0xBC - ( 0xBC - 0x22 ) * t ) << 8 ) | (uint32_t)( 0xC6 - ( 0xC6 - 0x30 ) * t );
	gap = 0xFF9EB2BC;
	SDL_LockTexture( gui_lcd_tex, NULL, &tp, &pitch );
	for ( y = 0; y < PNL_H * 4; y++ )
	{
		uint32_t *row = (uint32_t *)( (uint8_t *)tp + y * pitch );

		for ( x = 0; x < PNL_W * 4; x++ )
		{
			lit = ( gui_lcd[( y >> 2 ) * PNL_ROWB + ( x >> 5 )] >> ( 7 - ( ( x >> 2 ) & 7 ) ) ) & 1;
			row[x] = lit ? dot : ( ( x & 3 ) == 3 || ( y & 3 ) == 3 ? gap : bg );
		}
	}
	SDL_UnlockTexture( gui_lcd_tex );
	gui_rect( GUI_LCD_X0, GUI_LCD_Y0, GUI_LCD_X1 - GUI_LCD_X0, GUI_LCD_Y1 - GUI_LCD_Y0, ( gui_rgb ){ 0xA8, 0xBC, 0xC6 },
			  0xFF );
	SDL_RenderCopyF( gui_ren, gui_lcd_tex, NULL, &dst );
}

/**
  * @brief  Draw the whole panel into the frame texture and show it.
  *
  * @retval None.
  */
static void gui_draw( void )
{
	float a;
	int i, w, h;

	dw_lock();
	pnl_bitmap( gui_lcd );
	for ( i = 0; i < 8 * 16; i++ )
	{
		gui_leds[i / 16][i % 16] = (uint8_t)pnl_led( i / 16, i % 16 );
	}
	pnl_group_bytes( 0, gui_meter );
	gui_midi_count = sim_port_falls( 0, 1 );
	gui_midi_level = !( sim_port_out( 0 ) & 2 );
	gui_scsi_count = wd_activity();
	gui_contrast = pnl_contrast();
	dw_unlock();
	if ( !dw_powered )  // the panel holds its last states until the next power-on: show it dark
	{
		memset( gui_leds, 0, sizeof gui_leds );
		memset( gui_meter, 0, sizeof gui_meter );
	}

	SDL_SetRenderTarget( gui_ren, gui_frame_tex );
	SDL_SetRenderDrawBlendMode( gui_ren, SDL_BLENDMODE_BLEND );
	SDL_RenderCopy( gui_ren, gui_bg_tex, NULL, NULL );
	gui_draw_meters();
	gui_draw_lcd();
	for ( i = 0; i < GUI_NEL; i++ )
	{
		gui_draw_elem( &gui_el[i] );
	}
	gui_draw_activity();
	gui_draw_volume();
	// the finger dimple turns with the wheel (it starts up and right, where the artwork had it)
	a = gui_wheel_angle - 0.71f;
	gui_draw_dimple( GUI_WHEEL_X + GUI_DIMPLE_D * cosf( a ), GUI_WHEEL_Y + GUI_DIMPLE_D * sinf( a ) );
	if ( !dw_powered )  // the power switch out: a darker panel
	{
		gui_rect( 0, 0, GUI_PW / GUI_S, GUI_PH / GUI_S, ( gui_rgb ){ 0, 0, 0 }, 0x38 );
	}

	SDL_SetRenderTarget( gui_ren, NULL );
	SDL_GetRendererOutputSize( gui_ren, &w, &h );
	if ( (long)w * GUI_PH > (long)h * GUI_PW )
	{
		gui_view.h = h;
		gui_view.w = h * GUI_PW / GUI_PH;
	}
	else
	{
		gui_view.w = w;
		gui_view.h = w * GUI_PH / GUI_PW;
	}
	gui_view.x = ( w - gui_view.w ) / 2;
	gui_view.y = ( h - gui_view.h ) / 2;
	SDL_SetRenderDrawColor( gui_ren, 0x10, 0x10, 0x11, 0xFF );
	SDL_RenderClear( gui_ren );
	SDL_RenderCopy( gui_ren, gui_frame_tex, NULL, &gui_view );
	SDL_RenderPresent( gui_ren );
}

/**
  * @brief  Which key is at a panel position?
  *
  * @param  x Panel x, mm.
  * @param  y Panel y, mm.
  *
  * @retval The key, NULL if none.
  */
static gui_elem *gui_hit( float x, float y )
{
	gui_elem *e;
	float dx, dy;
	int i;

	for ( i = 0; i < GUI_NEL; i++ )
	{
		e = &gui_el[i];
		dx = ( x - e->x ) / ( e->w / 2 + 0.4f );
		dy = ( y - e->y ) / ( e->h / 2 + 0.4f );
		if ( e->kind == GUI_ROUND ? dx * dx + dy * dy <= 1 : fabsf( dx ) <= 1 && fabsf( dy ) <= 1 )
		{
			return e;
		}
	}
	return NULL;
}

/**
  * @brief  Press or release a key (sends it on the panel link).
  *
  * @param  e The key.
  * @param  down 1 pressed, 0 released.
  *
  * @retval None.
  */
static void gui_press( gui_elem *e, int down )
{
	if ( e->down == down )
	{
		return;
	}
	e->down = down;
	pnl_key( e->group, e->index, down );
}

/**
  * @brief  Window pixel to panel millimetres.
  *
  * @param  wx Window x.
  * @param  wy Window y.
  * @param  x Receives panel x, mm.
  * @param  y Receives panel y, mm.
  *
  * @retval None.
  */
static void gui_to_panel( int wx, int wy, float *x, float *y )
{
	int ow, oh, ww, wh;

	SDL_GetRendererOutputSize( gui_ren, &ow, &oh );
	SDL_GetWindowSize( gui_win, &ww, &wh );
	wx = ww ? wx * ow / ww : wx;  // high-DPI: window points to renderer pixels
	wy = wh ? wy * oh / wh : wy;
	*x = gui_view.w ? ( wx - gui_view.x ) * (float)GUI_PW / gui_view.w / GUI_S : 0;
	*y = gui_view.h ? ( wy - gui_view.y ) * (float)GUI_PH / gui_view.h / GUI_S : 0;
}

/**
  * @brief  The drive bay was clicked: put a disk image in the drive (audio bus, ID 0).
  *
  * @retval None.
  */
static void gui_choose_disk( void )
{
	char path[PLAT_PATH];

	snprintf( path, sizeof path, "%s", set_cfg.disk[1][0] );
	if ( sdlg_pick_file( gui_win, gui_draw, "Darwin drive: disk image (audio SCSI ID 0)", path, sizeof path, 0 ) != 0 )
	{
		return;
	}
	dw_lock();
	if ( sd_attach( 1, 0, path ) != 0 )
	{
		dw_unlock();
		gui_error( "That disk image cannot be opened." );
		return;
	}
	dw_unlock();
	snprintf( set_cfg.disk[1][0], SET_PATH, "%s", path );
	set_save();
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Show an error: message box, and on the console.
  *
  * @param  msg The message.
  *
  * @retval None.
  */
void gui_error( const char *msg )
{
	fprintf( stderr, "%s\n", msg );
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, DW_TITLE, msg, gui_win );
}

/**
  * @brief  Open the window and draw the panel artwork.
  *
  * @param  dir The program's folder (trailing backslash): UI.svg is looked for there.
  *
  * @retval 0 on success, -1 no display.
  */
int gui_init( const char *dir )
{
	int w, h;

	if ( SDL_Init( SDL_INIT_VIDEO ) != 0 )
	{
		return -1;
	}
	SDL_SetHint( SDL_HINT_RENDER_SCALE_QUALITY, "1" );
	SDL_SetHint( SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1" );
	gui_window_size( 0, &w, &h );
	dw_log( "gui: window %d x %d%s\n", w, h, set_cfg.ui_half ? " (half size)" : "" );
	gui_win = SDL_CreateWindow( DW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
								SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI );
	if ( !gui_win )
	{
		return -1;
	}
	gui_set_icon();
	gui_keys_load();
	gui_ren = SDL_CreateRenderer( gui_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE |
								  SDL_RENDERER_PRESENTVSYNC );
	if ( !gui_ren )
	{
		gui_ren = SDL_CreateRenderer( gui_win, -1, SDL_RENDERER_TARGETTEXTURE );
	}
	if ( !gui_ren )
	{
		return -1;
	}
	gui_frame_tex = SDL_CreateTexture( gui_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, GUI_PW, GUI_PH );
	gui_bg_tex = SDL_CreateTexture( gui_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, GUI_PW, GUI_PH );
	gui_lcd_tex = SDL_CreateTexture( gui_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, PNL_W * 4,
									 PNL_H * 4 );
	if ( !gui_frame_tex || !gui_bg_tex || !gui_lcd_tex )
	{
		return -1;
	}
	if ( gui_load_art( dir ) != 0 )
	{
		gui_error( "The panel artwork (UI.svg) could not be drawn." );
	}
	return 0;
}

/**
  * @brief  Size the window again after the size setting changed (centred on its display).
  *
  * @retval None.
  */
void gui_resize( void )
{
	int d, w, h;

	if ( !gui_win )
	{
		return;
	}
	d = SDL_GetWindowDisplayIndex( gui_win );
	d = d < 0 ? 0 : d;
	if ( SDL_GetWindowFlags( gui_win ) & SDL_WINDOW_MAXIMIZED )
	{
		SDL_RestoreWindow( gui_win );
	}
	gui_window_size( d, &w, &h );
	SDL_SetWindowSize( gui_win, w, h );
	SDL_SetWindowPosition( gui_win, SDL_WINDOWPOS_CENTERED_DISPLAY( d ), SDL_WINDOWPOS_CENTERED_DISPLAY( d ) );
}

/**
  * @brief  Handle input and draw one frame.
  *
  * @retval 0 keep running, 1 the window was closed.
  */
int gui_frame( void )
{
	static gui_elem *mouse_el;
	gui_elem *e;
	static int drag_wheel, drag_vol, vol0;
	static float vol_y0;
	static float drag_a;
	SDL_Event ev[64];
	float px, py, a, d;
	int mx, my;
	int n = 0, k, i, down;
	SDL_Keycode key;

	while ( n < 64 && SDL_PollEvent( &ev[n] ) )  // collect without the lock (Windows may block here)
	{
		n++;
	}
	dw_lock();
	for ( k = 0; k < n; k++ )
	{
		switch ( ev[k].type )
		{
		case SDL_WINDOWEVENT:
			if ( ev[k].window.event == SDL_WINDOWEVENT_FOCUS_LOST )  // a release may never arrive: let go of all keys
			{
				for ( i = 0; i < GUI_NEL; i++ )
				{
					if ( !gui_el[i].held )
					{
						gui_press( &gui_el[i], 0 );
					}
				}
				mouse_el = NULL;
				drag_wheel = 0;
			}
			break;
		case SDL_QUIT:
			dw_unlock();
			return 1;
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			if ( ev[k].key.repeat || !set_cfg.keyboard )  // settings, Keyboard tab: the shortcuts are off
			{
				break;
			}
			key = ev[k].key.keysym.sym;
			down = ev[k].type == SDL_KEYDOWN;
			for ( i = 0; key && i < GUI_NKEYS; i++ )
			{
				if ( gui_keys[i][0] != key && gui_keys[i][1] != key )
				{
					continue;
				}
				if ( i < GUI_NEL )
				{
					gui_el[i].held = 0;
					gui_press( &gui_el[i], down );
				}
				else if ( down )  // data wheel: left, right
				{
					pnl_wheel( i == GUI_NEL ? -1 : 1 );
					gui_wheel_angle += i == GUI_NEL ? -GUI_WHEEL_STEP : GUI_WHEEL_STEP;
				}
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
			gui_to_panel( ev[k].button.x, ev[k].button.y, &px, &py );
			if ( ev[k].button.button == SDL_BUTTON_RIGHT )  // hold a key down, or let it go
			{
				if ( ( e = gui_hit( px, py ) ) )
				{
					e->held = !e->held;
					gui_press( e, e->held );
				}
				break;
			}
			if ( ev[k].button.button != SDL_BUTTON_LEFT )
			{
				break;
			}
			if ( hypotf( px - GUI_POWER_X, py - GUI_POWER_Y ) <= GUI_POWER_R )
			{
				dw_power_request();
				break;
			}
			if ( px >= gui_logo.x && px < gui_logo.x + gui_logo.w && py >= gui_logo.y && py < gui_logo.y + gui_logo.h )
			{
				dw_unlock();
				sdlg_open( gui_win, gui_draw );
				gui_keys_load();  // the Keyboard tab may have changed them
				dw_lock();
				break;
			}
			if ( px >= gui_bay.x && px < gui_bay.x + gui_bay.w && py >= gui_bay.y && py < gui_bay.y + gui_bay.h )
			{
				dw_unlock();
				gui_choose_disk();
				dw_lock();
				break;
			}
			if ( ( mouse_el = gui_hit( px, py ) ) )
			{
				mouse_el->held = 0;  // a held key: this click lets it go on release
				gui_press( mouse_el, 1 );
			}
			else if ( hypotf( px - GUI_VOL_X, py - GUI_VOL_Y ) <= GUI_VOL_R )  // drag up / down to turn it
			{
				drag_vol = 1;
				vol_y0 = py;
				vol0 = set_cfg.phones_volume;
			}
			else if ( hypotf( px - GUI_WHEEL_X, py - GUI_WHEEL_Y ) <= GUI_WHEEL_R )
			{
				drag_wheel = 1;
				drag_a = atan2f( py - GUI_WHEEL_Y, px - GUI_WHEEL_X );
			}
			break;
		case SDL_MOUSEMOTION:
			if ( drag_vol )
			{
				gui_to_panel( ev[k].motion.x, ev[k].motion.y, &px, &py );
				gui_set_volume( vol0 + (int)( ( vol_y0 - py ) * 100 / GUI_VOL_DRAG ) );
			}
			if ( drag_wheel )  // one step per ~12 degrees, clockwise = increment
			{
				gui_to_panel( ev[k].motion.x, ev[k].motion.y, &px, &py );
				a = atan2f( py - GUI_WHEEL_Y, px - GUI_WHEEL_X );
				d = remainderf( a - drag_a, 2 * GUI_PI );
				while ( d > GUI_WHEEL_STEP )
				{
					pnl_wheel( 1 );
					gui_wheel_angle += GUI_WHEEL_STEP;
					drag_a += GUI_WHEEL_STEP;
					d -= GUI_WHEEL_STEP;
				}
				while ( d < -GUI_WHEEL_STEP )
				{
					pnl_wheel( -1 );
					gui_wheel_angle -= GUI_WHEEL_STEP;
					drag_a -= GUI_WHEEL_STEP;
					d += GUI_WHEEL_STEP;
				}
				drag_a = remainderf( drag_a, 2 * GUI_PI );
			}
			break;
		case SDL_MOUSEBUTTONUP:
			if ( ev[k].button.button == SDL_BUTTON_LEFT )
			{
				if ( mouse_el )
				{
					gui_press( mouse_el, 0 );
					mouse_el = NULL;
				}
				drag_wheel = 0;
				if ( drag_vol )
				{
					drag_vol = 0;
					set_save();
				}
			}
			break;
		case SDL_MOUSEWHEEL:
			SDL_GetMouseState( &mx, &my );
			gui_to_panel( mx, my, &px, &py );
			if ( ev[k].wheel.y && hypotf( px - GUI_VOL_X, py - GUI_VOL_Y ) <= GUI_VOL_R )  // over the knob: turn it
			{
				gui_set_volume( set_cfg.phones_volume + 5 * ( ev[k].wheel.y > 0 ? 1 : -1 ) );
				set_save();
			}
			else if ( ev[k].wheel.y )
			{
				i = ev[k].wheel.y > 0 ? 1 : -1;
				pnl_wheel( i );
				gui_wheel_angle += i * GUI_WHEEL_STEP;
			}
			break;
		default:
			break;
		}
	}
	dw_unlock();
	gui_draw();
	return 0;
}

/**
  * @brief  Save the panel as drawn (GUI_PW x GUI_PH) to a BMP file.
  *
  * @param  path The file.
  *
  * @retval 0 saved, -1 failed.
  */
int gui_screenshot( const char *path )
{
	SDL_Surface *s;
	int rc;

	if ( !gui_ren )
	{
		return -1;
	}
	gui_draw();
	s = SDL_CreateRGBSurfaceWithFormat( 0, GUI_PW, GUI_PH, 32, SDL_PIXELFORMAT_ARGB8888 );
	if ( !s )
	{
		return -1;
	}
	SDL_SetRenderTarget( gui_ren, gui_frame_tex );
	rc = SDL_RenderReadPixels( gui_ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch );
	SDL_SetRenderTarget( gui_ren, NULL );
	if ( rc == 0 )
	{
		rc = SDL_SaveBMP( s, path );
	}
	SDL_FreeSurface( s );
	return rc == 0 ? 0 : -1;
}

/**
  * @brief  Close the window.
  *
  * @retval None.
  */
void gui_shutdown( void )
{
	if ( gui_win )
	{
		SDL_DestroyWindow( gui_win );
	}
	SDL_Quit();
}

/**
  * @brief  How many panel controls take keyboard keys (the keys, the data wheel's two directions).
  *
  * @retval The number of controls.
  */
int gui_key_controls( void )
{
	return GUI_NKEYS;
}

/**
  * @brief  A control's name, as the settings window and Emu_Darwin.ini show it ("PLAY", "Track 1").
  *
  * @param  i Control 0 .. gui_key_controls() - 1.
  *
  * @retval The name.
  */
const char *gui_key_control( int i )
{
	if ( i < 0 || i >= GUI_NKEYS )
	{
		return "";
	}
	if ( !gui_key_names[i][0] )
	{
		snprintf( gui_key_names[i], sizeof gui_key_names[i], "%s%s",
				  i == GUI_NEL ? "Data wheel left" : i > GUI_NEL ? "Data wheel right" :
				  gui_el[i].group == 5 ? "Track " : gui_el[i].group == 2 ? "Keypad " : "",
				  i < GUI_NEL ? gui_el[i].label : "" );
	}
	return gui_key_names[i];
}

/**
  * @brief  A control's key as it comes without settings.
  *
  * @param  i Control.
  * @param  slot 0 the key, 1 the alternate key.
  *
  * @retval SDL key code, 0 for none.
  */
int32_t gui_key_default( int i, int slot )
{
	if ( i < 0 || i >= GUI_NKEYS )
	{
		return 0;
	}
	if ( i >= GUI_NEL )  // the data wheel: - and =
	{
		return slot ? 0 : i == GUI_NEL ? SDLK_MINUS : SDLK_EQUALS;
	}
	return slot ? gui_el[i].key2 : gui_el[i].key;
}

/**
  * @brief  A control's key now (the default, or what the settings changed it to).
  *
  * @param  i Control.
  * @param  slot 0 the key, 1 the alternate key.
  *
  * @retval SDL key code, 0 for none.
  */
int32_t gui_key_current( int i, int slot )
{
	return i < 0 || i >= GUI_NKEYS ? 0 : gui_keys[i][slot & 1];
}

/**
  * @brief  Take the key assignments from the settings (key.NAME / key2.NAME).
  *
  * A setting names the key as SDL does ("Space", "Keypad +", "Q"); a name
  * SDL does not know leaves the control without that key.
  *
  * @retval None.
  */
void gui_keys_load( void )
{
	char name[48];
	const char *v;
	int i, slot;

	for ( i = 0; i < GUI_NKEYS; i++ )
	{
		for ( slot = 0; slot < 2; slot++ )
		{
			snprintf( name, sizeof name, "%s.%.23s", slot ? "key2" : "key", gui_key_control( i ) );
			v = set_key_get( name );
			gui_keys[i][slot] = !v ? gui_key_default( i, slot ) : v[0] ? SDL_GetKeyFromName( v ) : 0;
			if ( v )
			{
				dw_log( "keys: %s = %s%s\n", name, v[0] ? v : "(none)",
						v[0] && !gui_keys[i][slot] ? " (not a key SDL knows: none)" : "" );
			}
		}
	}
}
