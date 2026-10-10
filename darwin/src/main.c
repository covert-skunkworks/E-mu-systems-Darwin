//  Description:
//
//		main.c -- E-mu Darwin emulator: start-up, run loop, debugging aids.
//
//		Two threads: the emulator runs in its
//		own thread in 100 us slices under a lock, paced to real time; the
//		main thread owns the window, polls input and draws at the display
//		rate. -headless runs the machine flat out with no window, for
//		scripted tests.
//
//		Files live next to the program: the firmware zips (or one named on the
//		command line; the settings window picks among them) and, per firmware,
//		darwin_nvram_<version>.bin, the 114 bytes of the clock chip's NVRAM
//		that hold the Darwin's settings. Each version resets settings another
//		version wrote, so they do not share one.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include "asic.h"
#include "audio.h"
#include "darwin.h"
#include "firmware.h"
#include "gui.h"
#include "machine.h"
#include "midi.h"
#include "panel.h"
#include "plat.h"
#include "rtc.h"
#include "scsidisk.h"
#include "setdlg.h"
#include "settings.h"
#include "sim68306.h"
#include "svg.h"
#include "v50.h"
#include "wd33c93.h"

#include "m68k.h"

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define DW_SLICE      ( DW_CPU_HZ / 10000 )  // 100 us
#define DW_HIST       256
#define DW_PROF_SLOTS 65536
#define DW_MAX_BPS    16
#define DW_FW_DIRS    8   // firmware folders: the program's and three above, each with its Firmware

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

char     dw_fw_version[64];
uint64_t dw_cycles;
int      dw_log_io;
uint64_t dw_trace_asic;

static FILE       *dw_log_fp;
static uint32_t    dw_hist[DW_HIST];
static unsigned    dw_hist_pos;
static uint32_t    dw_bps[DW_MAX_BPS];
static int         dw_nbps, dw_bp_hits[DW_MAX_BPS];
static int         dw_trace;
static uint32_t    dw_trace_lo, dw_trace_hi = 0xFFFFFF;
static uint32_t    dw_prof_pc[DW_PROF_SLOTS], dw_prof_n[DW_PROF_SLOTS];
static char        dw_dbg_line[256];     // debug console line being assembled
static int         dw_dbg_len;
static SDL_mutex  *dw_mtx;
static volatile int dw_quit, dw_done;
static uint64_t    dw_limit = UINT64_MAX;
static int         dw_turbo;
static int         dw_perf;           // -perf: timing figures once a wall second
static int         dw_fast_panel;     // -fastpanel
static uint64_t    dw_perf_wait, dw_perf_wait_max;  // GUI waits for the emulator lock (counter ticks)
static unsigned    dw_perf_midi, dw_perf_mtc;       // MIDI out bytes, of them MTC quarter frames
static SDL_atomic_t dw_gui_waiting;   // the GUI thread wants the emulator lock
volatile int       dw_powered = 1;
static volatile int dw_power_req;
static volatile int dw_fw_req;        // the firmware setting changed (settings window)
static char        dw_dir[PLAT_PATH];  // the program's folder, with a trailing separator
static char        dw_fwpath[PLAT_PATH];  // the running firmware
static char        dw_nvpath[PLAT_PATH];  // its settings memory (the RTC keeps a pointer to it)
static const char *dw_nvarg;          // -nvram: this one file, whatever the firmware
static int         dw_check = 1;      // 0: -nochecksum
static uint8_t     dw_rom_new[DW_ROM_SIZE];  // a firmware being loaded, apart until it passes
static uint64_t    dw_fw_at;          // -fwswitch: when (CPU cycles, 0 = never) ...
static char        dw_fw_test[DW_FW_NAME];  // ... to switch to this firmware
char               dw_fw_zip[DW_FW_NAME];
static const char *dw_input;        // -i: text typed into the debug console
static double      dw_input_t = 9;   // ... from this emulated time on
static const char *dw_ram_dump;      // -ram: 68306 DRAM written here at exit
static const char *dw_guishot;       // -guishot: the panel window as a BMP at exit
static const char *dw_lcd_out;       // -lcd: the panel's display as a PGM at exit
static double      dw_snap;          // -snap: seconds between display checks (0 = off)
static uint64_t    dw_next_snap;

// -k: scripted panel input, in time order; group -1 = data wheel by index steps
typedef struct
{
	uint64_t at;
	int      group, index, down;
} dw_key_event;

// dw_find_firmware's search of one folder
typedef struct
{
	const char *name;              // the zip wanted, "" for the newest
	char        best[DW_FW_NAME];  // what was found so far
} dw_fw_search;

// dw_firmware_list's collection
typedef struct
{
	char ( *names )[DW_FW_NAME];
	int    max, k;
} dw_fw_names;

static dw_key_event dw_keys[256];
static int          dw_nkeys;

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static void dw_dump_regs( void );
static void dw_backtrace( void );
static void dw_dump_profile( void );
static void dw_write_lcd( const char *path, const uint8_t *bm );
static int dw_svg2bmp( const char *in, const char *out, int w );
static void dw_dbg_tx( int chan, uint8_t byte, void *ctx );
static void dw_cpu_tx( int chan, uint8_t byte, void *ctx );
static int dw_fw_match( const char *f );
static int dw_fw_dirs( char dirs[DW_FW_DIRS][PLAT_PATH] );
static void dw_fw_pick( const char *f, int is_dir, void *ctx );
static void dw_fw_collect( const char *f, int is_dir, void *ctx );
static int dw_find_firmware( const char *name, char *out, size_t n );
static int dw_load_firmware( const char *path, char *err, size_t n );
static void dw_switch_firmware( void );
static int dw_run( void *arg );
static void dw_usage( void );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Log the CPU registers.
  *
  * @retval None.
  */
static void dw_dump_regs( void )
{
	int i;

	for ( i = 0; i < 16; i++ )
	{
		dw_log( "%c%d=%08X%s", i < 8 ? 'D' : 'A', i & 7,
				m68k_get_reg( NULL, (m68k_register_t)( M68K_REG_D0 + i ) ), ( i & 7 ) == 7 ? "\n" : " " );
	}
	dw_log( "SR=%04X USP=%08X ISP=%08X\n", m68k_get_reg( NULL, M68K_REG_SR ),
			m68k_get_reg( NULL, M68K_REG_USP ), m68k_get_reg( NULL, M68K_REG_ISP ) );
}

/**
  * @brief  Log the 20 most-executed PCs and a LINK A6 backtrace.
  *
  * @retval None.
  */
static void dw_dump_profile( void )
{
	char buf[128];
	int k, i, best;

	for ( i = 0, k = 0; i < DW_PROF_SLOTS; i++ )
	{
		k += dw_prof_n[i] != 0;
	}
	dw_log( "\n--- PC profile (top 20 of %d PCs) ---\n", k );
	for ( k = 0; k < 20; k++ )
	{
		best = -1;
		for ( i = 0; i < DW_PROF_SLOTS; i++ )
		{
			if ( dw_prof_n[i] && ( best < 0 || dw_prof_n[i] > dw_prof_n[best] ) )
			{
				best = i;
			}
		}
		if ( best < 0 )
		{
			break;
		}
		m68k_disassemble( buf, dw_prof_pc[best], M68K_CPU_TYPE_68000 );
		dw_log( "%8u  %06X  %s\n", dw_prof_n[best], dw_prof_pc[best], buf );
		dw_prof_n[best] = 0;
	}
	dw_log( "cycles: %llu (%.2f s emulated)\n", (unsigned long long)dw_cycles,
			(double)dw_cycles / DW_CPU_HZ );
	dw_dump_regs();
	dw_backtrace();
}

/**
  * @brief  Log the return addresses along the LINK A6 frame chain.
  *
  * @retval None.
  */
static void dw_backtrace( void )
{
	uint32_t fp = m68k_get_reg( NULL, M68K_REG_A6 );
	int k;

	dw_log( "backtrace:" );
	for ( k = 0; k < 16 && fp >= DW_RAM_BASE && fp < DW_RAM_BASE + DW_RAM_SIZE - 8; k++ )
	{
		dw_log( " %06X", m68k_read_disassembler_32( fp + 4 ) & 0xFFFFFF );
		fp = m68k_read_disassembler_32( fp );
	}
	dw_log( "\n" );
}

/**
  * @brief  Write a panel bitmap as a PGM, 3x enlarged, dark dots on light glass.
  *
  * @param  path The file.
  * @param  bm The bitmap, PNL_ROWB bytes per row, MSB = left.
  *
  * @retval None.
  */
static void dw_write_lcd( const char *path, const uint8_t *bm )
{
	FILE *f = fopen( path, "wb" );
	int x, y, s;

	if ( !f )
	{
		return;
	}
	fprintf( f, "P5\n%d %d\n255\n", PNL_W * 3, PNL_H * 3 );
	for ( y = 0; y < PNL_H * 3; y++ )
	{
		for ( x = 0; x < PNL_W * 3; x++ )
		{
			s = ( bm[( y / 3 ) * PNL_ROWB + ( x / 3 ) / 8] >> ( 7 - ( x / 3 ) % 8 ) ) & 1;
			fputc( s ? 30 : 200, f );
		}
	}
	fclose( f );
}

/**
  * @brief  Render an SVG file to a BMP (to check the panel artwork renderer).
  *
  * @param  in The SVG file.
  * @param  out The BMP file.
  * @param  w Width in pixels; the height follows the document's aspect.
  *
  * @retval 0 written, -1 failed.
  */
static int dw_svg2bmp( const char *in, const char *out, int w )
{
	SDL_Surface *s;
	FILE *f = fopen( in, "rb" );
	char *doc;
	long len;
	double dw, dh;
	int h, rc = -1;

	if ( !f )
	{
		return -1;
	}
	fseek( f, 0, SEEK_END );
	len = ftell( f );
	fseek( f, 0, SEEK_SET );
	doc = malloc( (size_t)len );
	if ( doc && fread( doc, 1, (size_t)len, f ) == (size_t)len && svg_size( doc, (size_t)len, &dw, &dh ) == 0 )
	{
		h = (int)( w * dh / dw + 0.5 );
		s = SDL_CreateRGBSurfaceWithFormat( 0, w, h, 32, SDL_PIXELFORMAT_ARGB8888 );
		if ( s )
		{
			SDL_FillRect( s, NULL, 0xFFFFFFFF );
			if ( svg_render( doc, (size_t)len, s->pixels, w, h ) == 0 && SDL_SaveBMP( s, out ) == 0 )
			{
				rc = 0;
			}
			SDL_FreeSurface( s );
		}
	}
	free( doc );
	fclose( f );
	return rc;
}

/**
  * @brief  A byte the debug DUART transmits: channel B is the debug console.
  *
  * @param  chan 0 = A, 1 = B.
  * @param  byte The byte.
  * @param  ctx Unused.
  *
  * @retval None.
  */
static void dw_dbg_tx( int chan, uint8_t byte, void *ctx )
{
	(void)ctx;
	if ( byte == '\n' || byte == '\r' || dw_dbg_len >= (int)sizeof dw_dbg_line - 1 )
	{
		if ( dw_dbg_len )
		{
			dw_dbg_line[dw_dbg_len] = 0;
			dw_log( "[dbg%c] %s\n", 'A' + chan, dw_dbg_line );
			dw_dbg_len = 0;
		}
		return;
	}
	if ( byte >= 0x20 && byte < 0x7F )
	{
		dw_dbg_line[dw_dbg_len++] = (char)byte;
	}
}

/**
  * @brief  A byte the 68306's DUART transmits: A = remote link, B = MIDI out.
  *
  * @param  chan 0 = A, 1 = B.
  * @param  byte The byte.
  * @param  ctx Unused.
  *
  * @retval None.
  */
static void dw_cpu_tx( int chan, uint8_t byte, void *ctx )
{
	(void)ctx;
	if ( dw_log_io )
	{
		dw_log( "%s out: %02X\n", chan ? "MIDI" : "remote", byte );
	}
	else if ( dw_trace_asic && dw_cycles >= dw_trace_asic )
	{
		dw_log( "%10.6f %s out %02X\n", (double)dw_cycles / DW_CPU_HZ, chan ? "MIDI" : "remote", byte );
	}
	if ( chan == 0 )
	{
		pnl_rx( byte );
	}
	else
	{
		dw_perf_midi++;
		if ( dw_perf && byte == 0xF1 )  // MIDI time code: 120 quarter frames a second at 30 fps
		{
			dw_perf_mtc++;
		}
	}
	if ( chan == 1 )
	{
		midi_out_byte( byte );
	}
}

/**
  * @brief  Is a file name a firmware zip, emu_darwin_v*.zip (any case)?
  *
  * @param  f The file name.
  *
  * @retval 1 it is.
  */
static int dw_fw_match( const char *f )
{
	size_t n = strlen( f );

	return n > 16 && !plat_strnicmp( f, "emu_darwin_v", 12 ) && !plat_stricmp( f + n - 4, ".zip" );
}

/**
  * @brief  The folders searched for firmware, in order: the program's folder and its
  *         Firmware subfolder, then the same for up to three folders above.
  *
  * @param  dirs Receives the folders, each with a trailing separator.
  *
  * @retval The number of folders.
  */
static int dw_fw_dirs( char dirs[DW_FW_DIRS][PLAT_PATH] )
{
	char up[PLAT_PATH];
	int i, n = 0;

	snprintf( up, sizeof up, "%s", dw_dir );
	for ( i = 0; i < DW_FW_DIRS / 2; i++ )
	{
		if ( snprintf( dirs[n], PLAT_PATH, "%s", up ) < PLAT_PATH )  // a folder too deep to name is skipped
		{
			n++;
		}
		if ( snprintf( dirs[n], PLAT_PATH, "%sFirmware%c", up, PLAT_SEP ) < PLAT_PATH )
		{
			n++;
		}
		if ( strlen( up ) + 4 < sizeof up )
		{
			snprintf( up + strlen( up ), sizeof up - strlen( up ), "..%c", PLAT_SEP );
		}
	}
	return n;
}

/**
  * @brief  plat_list_dir callback for dw_find_firmware: keep the wanted (or newest) zip.
  *
  * @param  f A file in the folder.
  * @param  is_dir 1 for a folder.
  * @param  ctx The search (dw_fw_search).
  *
  * @retval None.
  */
static void dw_fw_pick( const char *f, int is_dir, void *ctx )
{
	dw_fw_search *s = ctx;

	if ( is_dir || !dw_fw_match( f ) || strlen( f ) >= sizeof s->best )
	{
		return;
	}
	if ( s->name[0] ? !plat_stricmp( f, s->name ) : strcmp( f, s->best ) > 0 )
	{
		snprintf( s->best, sizeof s->best, "%s", f );
	}
}

/**
  * @brief  plat_list_dir callback for dw_firmware_list: add each firmware zip once.
  *
  * @param  f A file in the folder.
  * @param  is_dir 1 for a folder.
  * @param  ctx The collection (dw_fw_names).
  *
  * @retval None.
  */
static void dw_fw_collect( const char *f, int is_dir, void *ctx )
{
	dw_fw_names *l = ctx;
	int i;

	if ( is_dir || !dw_fw_match( f ) || strlen( f ) >= DW_FW_NAME )
	{
		return;
	}
	for ( i = 0; i < l->k && plat_stricmp( l->names[i], f ); i++ )
	{
	}
	if ( i == l->k && l->k < l->max )
	{
		snprintf( l->names[l->k++], DW_FW_NAME, "%s", f );
	}
}

/**
  * @brief  Find a firmware zip in the firmware folders (dw_fw_dirs), the first folder first.
  *
  * Version names sort as text (v2_02 < v2_50), so without a name the
  * newest zip of the first folder that holds any is taken. Names compare
  * without case, so Linux and macOS find the zips however they are spelt.
  *
  * @param  name The zip's file name, "" for the newest.
  * @param  out Receives the path.
  * @param  n Room in out.
  *
  * @retval 0 found, -1 none.
  */
static int dw_find_firmware( const char *name, char *out, size_t n )
{
	char dirs[DW_FW_DIRS][PLAT_PATH];
	dw_fw_search s;
	int d, nd = dw_fw_dirs( dirs );

	for ( d = 0; d < nd; d++ )
	{
		s.name = name;
		s.best[0] = 0;
		if ( plat_list_dir( dirs[d], dw_fw_pick, &s ) != 0 || !s.best[0] )
		{
			continue;
		}
		if ( snprintf( out, n, "%s%s", dirs[d], s.best ) < (int)n )
		{
			return 0;
		}
	}
	return -1;
}

/**
  * @brief  Load a firmware into the ROM and open its own settings memory.
  *
  * The image is checked apart from the running ROM, so a bad file leaves
  * the running firmware untouched. Each firmware keeps its own NVRAM file,
  * darwin_nvram_<version>.bin (emu_darwin_v2_02.zip: darwin_nvram_v2_02.bin),
  * because each resets settings written by another version. The old
  * shared darwin_nvram.bin becomes the file of the first firmware that runs.
  * -nvram names one file for every firmware.
  *
  * @param  path The zip (or 1 MB .bin).
  * @param  err Receives a message on failure.
  * @param  n Room in err.
  *
  * @retval 0 loaded, else fw_load's error code.
  */
static int dw_load_firmware( const char *path, char *err, size_t n )
{
	char tag[DW_FW_NAME], nv[PLAT_PATH], old[PLAT_PATH + 32], *s;
	const char *base;
	fw_info info;
	int rc;

	base = strrchr( path, '\\' ) ? strrchr( path, '\\' ) + 1 : strrchr( path, '/' ) ? strrchr( path, '/' ) + 1 : path;
	snprintf( tag, sizeof tag, "%s", plat_strnicmp( base, "emu_darwin_", 11 ) ? base : base + 11 );
	if ( ( s = strrchr( tag, '.' ) ) )
	{
		*s = 0;
	}
	if ( snprintf( nv, sizeof nv, "%s", dw_nvarg ? dw_nvarg : "" ) >= (int)sizeof nv ||
		 ( !dw_nvarg && snprintf( nv, sizeof nv, "%sdarwin_nvram_%s.bin", dw_dir, tag ) >= (int)sizeof nv ) )
	{
		snprintf( err, n, "the settings memory's path is too long" );
		return -9;
	}
	rc = fw_load( path, dw_rom_new, &info, dw_check, err, n );
	if ( rc != 0 )
	{
		return rc;
	}

	// the image passed: from here on the firmware is switched
	memcpy( dw_rom, dw_rom_new, sizeof dw_rom );
	pnl_rom_changed();
	dw_log( "firmware %s: %s, %s, ROM checksum %02X (stored %02X) OK, MSB sum %04X, LSB sum %04X\n",
			path, info.source, info.version, info.computed_sum, info.stored_sum, info.sum_msb,
			info.sum_lsb );
	snprintf( dw_fw_version, sizeof dw_fw_version, "%s", info.version );
	snprintf( dw_fwpath, sizeof dw_fwpath, "%s", path );
	snprintf( dw_fw_zip, sizeof dw_fw_zip, "%s", base );
	snprintf( dw_nvpath, sizeof dw_nvpath, "%s", nv );
	snprintf( old, sizeof old, "%sdarwin_nvram.bin", dw_dir );
	if ( !dw_nvarg && !plat_exists( dw_nvpath ) && plat_exists( old ) && rename( old, dw_nvpath ) == 0 )
	{
		dw_log( "settings NVRAM: %s renamed to %s\n", old, dw_nvpath );
	}
	rc = rtc_init( dw_nvpath );
	dw_log( "settings NVRAM %s: %s\n", dw_nvpath,
			rc == 0 ? "loaded" : rc > 0 ? "none yet (the firmware restores its defaults)" : "unreadable" );
	return 0;
}

/**
  * @brief  Run the firmware the settings name, if it is not the one running: save the
  *         running one's settings memory, load the other, power the unit up again.
  *
  * @retval None.
  */
static void dw_switch_firmware( void )
{
	char path[PLAT_PATH], err[512];

	if ( dw_find_firmware( set_cfg.firmware, path, sizeof path ) != 0 )
	{
		dw_log( "firmware %s not found: %s keeps running\n", set_cfg.firmware, dw_fwpath );
		return;
	}
	if ( !plat_stricmp( path, dw_fwpath ) )
	{
		return;
	}
	dw_lock();
	rtc_save();
	if ( dw_load_firmware( path, err, sizeof err ) != 0 )
	{
		dw_log( "firmware %s rejected: %s; %s keeps running\n", path, err, dw_fwpath );
	}
	else if ( dw_powered )
	{
		mc_power_on();  // switched off, it starts with the new firmware at the next power-on
	}
	dw_unlock();
}

/**
  * @brief  The emulator thread: run slices, pace to real time.
  *
  * @param  arg Unused.
  *
  * @retval 0.
  */
static int dw_run( void *arg )
{
	uint64_t freq = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter(), next_save = 0;
	double emu_t, real_t;
	uint32_t pc, h, s;
	uint8_t midi_b;
	int k;

	(void)arg;
	while ( dw_cycles < dw_limit && !dw_quit )
	{
		if ( dw_fw_at && dw_cycles >= dw_fw_at )  // -fwswitch: what the settings window's firmware list does
		{
			dw_fw_at = 0;
			snprintf( set_cfg.firmware, SET_DEV, "%s", dw_fw_test );
			dw_switch_firmware();
		}
		if ( !dw_powered )
		{
			SDL_Delay( 5 );
			t0 = SDL_GetPerformanceCounter() - (uint64_t)( (double)dw_cycles / DW_CPU_HZ * freq );
			continue;
		}
		if ( dw_mtx )
		{
			SDL_LockMutex( dw_mtx );
		}
		mc_run( DW_SLICE );
		while ( duart_rx_space( &mc_duart, 1 ) > 0 && midi_in_byte( &midi_b ) )  // host MIDI in: channel B
		{
			duart_rx_push( &mc_duart, 1, midi_b );
		}
		while ( dw_nkeys > 0 && dw_cycles >= dw_keys[0].at )  // -k: scripted panel keys
		{
			if ( dw_keys[0].group < 0 )
			{
				pnl_wheel( dw_keys[0].index );
			}
			else
			{
				pnl_key( dw_keys[0].group, dw_keys[0].index, dw_keys[0].down );
			}
			memmove( dw_keys, dw_keys + 1, (size_t)--dw_nkeys * sizeof dw_keys[0] );
		}
		if ( dw_snap > 0 && dw_cycles >= dw_next_snap )  // -snap: frames while the display changes
		{
			static uint8_t last[PNL_ROWB * PNL_H];
			uint8_t bm[PNL_ROWB * PNL_H];
			char name[64];

			dw_next_snap = dw_cycles + (uint64_t)( dw_snap * DW_CPU_HZ );
			pnl_bitmap( bm );
			if ( memcmp( bm, last, sizeof bm ) != 0 )
			{
				memcpy( last, bm, sizeof bm );
				snprintf( name, sizeof name, "snap_%07.2f.pgm", (double)dw_cycles / DW_CPU_HZ );
				dw_write_lcd( name, bm );
			}
		}
		if ( dw_input && *dw_input && dw_cycles >= (uint64_t)( dw_input_t * DW_CPU_HZ ) &&
			 duart_rx_space( &mc_dbg_duart, 1 ) > 0 )
		{
			duart_rx_push( &mc_dbg_duart, 1, (uint8_t)( *dw_input == '\n' ? '\r' : *dw_input ) );
			dw_input++;
		}
		if ( dw_cycles >= next_save )  // settings reach the disk within 2 s of a change
		{
			next_save = dw_cycles + 2ull * DW_CPU_HZ;
			rtc_save();
		}
		pc = m68k_get_reg( NULL, M68K_REG_PC );
		h = ( pc * 2654435761u ) >> 16;
		for ( k = 0; k < 8; k++ )
		{
			s = ( h + (uint32_t)k ) & ( DW_PROF_SLOTS - 1 );
			if ( !dw_prof_n[s] || dw_prof_pc[s] == pc )
			{
				dw_prof_pc[s] = pc;
				dw_prof_n[s]++;
				break;
			}
		}
		if ( dw_mtx )
		{
			SDL_UnlockMutex( dw_mtx );
			// the mutex is not fair: while catching up, this thread would take it straight back and
			// starve the GUI for the whole catch-up. Let a waiting GUI thread have it first.
			while ( SDL_AtomicGet( &dw_gui_waiting ) > 0 && !dw_quit )
			{
				SDL_Delay( 0 );
			}
		}
		if ( !dw_turbo && ( dw_cycles % ( DW_CPU_HZ / 1000 ) ) < DW_SLICE )
		{
			emu_t = (double)dw_cycles / DW_CPU_HZ;
			real_t = (double)( SDL_GetPerformanceCounter() - t0 ) / freq;
			if ( emu_t > real_t )
			{
				SDL_Delay( (Uint32)( ( emu_t - real_t ) * 1000 ) );
			}
			else if ( real_t - emu_t > 0.25 )  // fell behind: do not try to catch up
			{
				t0 = SDL_GetPerformanceCounter() - (uint64_t)( emu_t * freq );
			}
		}
	}
	dw_done = 1;
	return 0;
}

/**
  * @brief  Print the options and exit.
  *
  * @retval None (does not return).
  */
static void dw_usage( void )
{
	fprintf( stderr,
		"usage: Emu_Darwin [options] [firmware.zip | firmware.bin]\n"
		"  Without a firmware argument the newest emu_darwin_v*.zip next to the\n"
		"  program (or in ..\\Firmware) is used. It is unpacked in memory and\n"
		"  checked: zip CRC-32s, MSB/LSB chips vs the combined image, ROM checksum.\n"
		"  -s SECONDS   stop after that much emulated time\n"
		"  -headless    no window, run flat out\n"
		"  -turbo       window, but do not pace to real time\n"
		"  -l FILE      log file (default: Emu_Darwin.log next to the program)\n"
		"  -nvram FILE  settings NVRAM, for any firmware (default: one per firmware next\n"
		"               to the program, darwin_nvram_<version>.bin, e.g. _v2_50)\n"
		"  -nochecksum  run an image whose ROM checksum does not match\n"
		"  -nodebug     no debug DUART at B00000 (as a production unit)\n"
		"  -hd [B:]ID:FILE  disk image on SCSI bus B (1 = audio, default; 0 = host) at ID 0-7\n"
		"  -newdisk FILE:MB  create a blank (sparse) disk image and exit; the Darwin\n"
		"               formats 275 MB and up, 2 GB at most\n"
		"  -i TEXT      type TEXT into the debug console (\\n = Enter), from -it SEC on (default 9)\n"
		"  -io          log every unmapped / ASIC access\n"
		"  -t LO-HI     trace instructions in a hex PC range\n"
		"  -bp ADDR     breakpoint (hex): PC history and registers on the first hit\n"
		"  -wlog LO-HI  log 68k writes in a hex address range\n"
		"  -k LIST      scripted panel input, comma separated: T:G.I[+HOLD] presses\n"
		"               key group G index I at T seconds for HOLD s (0.15), T:wN\n"
		"               turns the wheel N steps; F1-F6 = 3.0-3.5, EXIT = 3.6,\n"
		"               PLAY 4.0, STOP 4.1, REC 4.2, REW 4.3, F.FWD 4.4, RTZ 4.5\n"
		"  -snap SEC    every SEC, write snap_TTTT.TT.pgm if the LCD changed\n"
		"  -lcd FILE    write the LCD as PGM at exit\n"
		"  -guishot FILE  write the panel window as BMP at exit\n"
		"  -ram FILE    dump 68k RAM, then V50 memory, at exit\n"
		"  -svg2bmp IN OUT WIDTH  render an SVG file to a BMP and exit\n"
		"  -smoke FILE  open the panel window, save it as a BMP and exit (no firmware needed)\n"
		"  -dlgshot PREFIX  save each settings page as PREFIXn.bmp and exit\n"
		"  -fastpanel   run the front-panel link 16 x faster than the hardware\n"
		"  -fwswitch SEC:ZIP  at SEC, switch to another firmware zip (by file name) as\n"
		"               the settings window does: a restart with its own NVRAM\n"
		"  -perf        log timing once a second: emulated vs wall time, frame rate,\n"
		"               GUI lock waits, MIDI out and MIDI time code rate\n"
		"  -wav FILE    also write the mix outputs (L, R) to a WAV file\n"
		"  -wavin FILE  feed analog inputs 1 and 2 from a 16-bit WAV file\n" );
	exit( 1 );
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  printf to the log file (or stderr).
  *
  * @param  fmt printf format.
  *
  * @retval None.
  */
void dw_log( const char *fmt, ... )
{
	va_list ap;

	va_start( ap, fmt );
	vfprintf( dw_log_fp ? dw_log_fp : stderr, fmt, ap );
	va_end( ap );
	if ( dw_log_fp )
	{
		fflush( dw_log_fp );
	}
}

/**
  * @brief  The PC of the instruction being executed.
  *
  * @retval The PC.
  */
uint32_t dw_pc( void ) { return m68k_get_reg( NULL, M68K_REG_PPC ); }

/**
  * @brief  Take the emulator lock (GUI thread, around input and drawing).
  *
  * @retval None.
  */
void dw_lock( void )
{
	uint64_t t;

	if ( dw_mtx )
	{
		t = SDL_GetPerformanceCounter();
		SDL_AtomicIncRef( &dw_gui_waiting );  // the emulator thread steps aside (dw_run)
		SDL_LockMutex( dw_mtx );
		SDL_AtomicDecRef( &dw_gui_waiting );
		t = SDL_GetPerformanceCounter() - t;  // -perf: how long the GUI waited for the emulator
		dw_perf_wait += t;
		dw_perf_wait_max = t > dw_perf_wait_max ? t : dw_perf_wait_max;
	}
}

/**
  * @brief  Release the emulator lock.
  *
  * @retval None.
  */
void dw_unlock( void )
{
	if ( dw_mtx )
	{
		SDL_UnlockMutex( dw_mtx );
	}
}

/**
  * @brief  The panel's power button was pressed.
  *
  * @retval None.
  */
void dw_power_request( void ) { dw_power_req = 1; }

/**
  * @brief  The firmware setting changed: restart the unit with it (as a power cycle).
  *
  * @retval None.
  */
void dw_firmware_request( void ) { dw_fw_req = 1; }

/**
  * @brief  The firmware zips the program can find (the folders of dw_fw_dirs).
  *
  * @param  names Receives the file names, sorted, each found once.
  * @param  max Room in names.
  *
  * @retval The number found.
  */
int dw_firmware_list( char names[][DW_FW_NAME], int max )
{
	char dirs[DW_FW_DIRS][PLAT_PATH], t[DW_FW_NAME];
	dw_fw_names l = { names, max, 0 };
	int d, nd = dw_fw_dirs( dirs ), i, j;

	for ( d = 0; d < nd; d++ )
	{
		plat_list_dir( dirs[d], dw_fw_collect, &l );
	}
	for ( i = 1; i < l.k; i++ )  // a handful of names: insertion sort
	{
		snprintf( t, sizeof t, "%s", names[i] );
		for ( j = i; j > 0 && strcmp( names[j - 1], t ) > 0; j-- )
		{
			snprintf( names[j], DW_FW_NAME, "%s", names[j - 1] );
		}
		snprintf( names[j], DW_FW_NAME, "%s", t );
	}
	return l.k;
}

/**
  * @brief  Musashi instruction hook: PC history, breakpoints, trace.
  *
  * @param  pc The instruction's address.
  *
  * @retval None.
  */
void emu_instr_hook( unsigned int pc )
{
	char buf[128];
	unsigned k;
	int b, r;

	if ( pnl_active() )  // the front panel's processor
	{
		pnl_hook( pc );
		return;
	}
	dw_hist[dw_hist_pos++ % DW_HIST] = pc;
	for ( b = 0; b < dw_nbps; b++ )
	{
		if ( pc == dw_bps[b] && dw_bp_hits[b]++ == 0 )
		{
			dw_log( "=== breakpoint %06X at %.3f s; last PCs:\n", pc, (double)dw_cycles / DW_CPU_HZ );
			for ( k = dw_hist_pos - 48; k != dw_hist_pos; k++ )
			{
				m68k_disassemble( buf, dw_hist[k % DW_HIST], M68K_CPU_TYPE_68000 );
				dw_log( "  %06X  %s\n", dw_hist[k % DW_HIST], buf );
			}
			dw_dump_regs();
			dw_backtrace();
		}
	}
	if ( dw_trace && pc >= dw_trace_lo && pc <= dw_trace_hi )
	{
		m68k_disassemble( buf, pc, M68K_CPU_TYPE_68000 );
		dw_log( "%06X  %-34s", pc, buf );
		for ( r = 0; r < 16; r++ )
		{
			dw_log( "%c%d=%08X ", r < 8 ? 'D' : 'A', r & 7,
					m68k_get_reg( NULL, (m68k_register_t)( M68K_REG_D0 + r ) ) );
		}
		dw_log( "\n" );
	}
}

/**
  * @brief  Program entry.
  *
  * @param  argc Argument count.
  * @param  argv Arguments.
  *
  * @retval Exit status.
  */
int main( int argc, char **argv )
{
	char dir[PLAT_PATH], fwpath[PLAT_PATH] = "", up[PLAT_PATH + 32], err[512];
	const char *fwarg = NULL;
	double seconds = 0;
	int headless = 0, i, rc, frames = 0;
	uint64_t perf_t0 = 0, perf_emu0 = 0;
	unsigned perf_scsi0 = 0, perf_led0 = 0;
	SDL_Thread *th;

	SDL_SetMainReady();
	plat_console_attach();
	plat_exe_dir( dir, sizeof dir );
	if ( strstr( dir, "/AppTranslocation/" ) )
	{
		// macOS runs a downloaded app that still carries its quarantine mark from a hidden,
		// read-only copy: no Firmware folder beside it, and nowhere to keep the settings
		gui_error( "macOS is running this copy from a hidden, read-only place (it was downloaded and is "
				   "not notarized), so it cannot find its Firmware folder or keep its settings.\n\n"
				   "In Terminal, in the unpacked folder, run:\n\n"
				   "    xattr -dr com.apple.quarantine .\n\n"
				   "then start Emu_Darwin again." );
		return 1;
	}
	snprintf( dw_dir, sizeof dw_dir, "%s", dir );
	snprintf( up, sizeof up, "%sEmu_Darwin.ini", dir );
	set_load( up );  // before the command line, which overrides it
	au_set_phones( set_cfg.phones_volume / 100.0f );  // the headphone knob as it was left
	mc_dbg_fitted = set_cfg.debug_duart;
	for ( i = 1; i < argc; i++ )
	{
		if ( !strcmp( argv[i], "-s" ) && i + 1 < argc )
		{
			seconds = atof( argv[++i] );
		}
		else if ( !strcmp( argv[i], "-l" ) && i + 1 < argc )
		{
			dw_log_fp = fopen( argv[++i], "w" );
		}
		else if ( !strcmp( argv[i], "-headless" ) )
		{
			headless = 1;
			dw_turbo = 1;
		}
		else if ( !strcmp( argv[i], "-turbo" ) )
		{
			dw_turbo = 1;
		}
		else if ( !strcmp( argv[i], "-nvram" ) && i + 1 < argc )
		{
			dw_nvarg = argv[++i];
		}
		else if ( !strcmp( argv[i], "-nochecksum" ) )
		{
			dw_check = 0;
		}
		else if ( !strcmp( argv[i], "-nodebug" ) )
		{
			mc_dbg_fitted = 0;
		}
		else if ( !strcmp( argv[i], "-i" ) && i + 1 < argc )
		{
			char *t = strdup( argv[++i] ), *d = t, *q;

			for ( q = t; *q; q++ )  // \n in the argument = Enter
			{
				if ( q[0] == '\\' && q[1] == 'n' )
				{
					*d++ = '\n';
					q++;
				}
				else
				{
					*d++ = *q;
				}
			}
			*d = 0;
			dw_input = t;
		}
		else if ( !strcmp( argv[i], "-hd" ) && i + 1 < argc )  // [BUS:]ID:FILE, bus 1 (audio) by default
		{
			const char *spec = argv[++i];
			int bus = 1, id;

			if ( spec[0] >= '0' && spec[0] <= '1' && spec[1] == ':' && spec[2] >= '0' && spec[2] <= '7' &&
				 spec[3] == ':' )
			{
				bus = spec[0] - '0';
				spec += 2;
			}
			id = spec[0] - '0';
			if ( id < 0 || id > 7 || spec[1] != ':' || sd_attach( bus, id, spec + 2 ) != 0 )
			{
				fprintf( stderr, "-hd %s: expected [BUS:]ID:FILE with an existing image\n", argv[i] );
			}
		}
		else if ( !strcmp( argv[i], "-newdisk" ) && i + 1 < argc )  // FILE:MB
		{
			char path[PLAT_PATH];
			unsigned mb = 0;
			char *colon;

			snprintf( path, sizeof path, "%s", argv[++i] );
			colon = strrchr( path, ':' );
			if ( colon && colon > path + 1 )
			{
				*colon = 0;
				mb = (unsigned)atoi( colon + 1 );
			}
			if ( mb == 0 || sd_create( path, mb ) != 0 )
			{
				fprintf( stderr, "-newdisk FILE:MB failed for %s\n", argv[i] );
				return 1;
			}
			printf( "created %s, %u MB\n", path, mb );
			return 0;
		}
		else if ( !strcmp( argv[i], "-wav" ) && i + 1 < argc )
		{
			if ( au_wav_out( argv[++i] ) != 0 )
			{
				fprintf( stderr, "-wav: cannot write %s\n", argv[i] );
			}
		}
		else if ( !strcmp( argv[i], "-wavin" ) && i + 1 < argc )
		{
			if ( au_wav_in( argv[++i] ) != 0 )
			{
				fprintf( stderr, "-wavin: %s is not a 16-bit PCM WAV file\n", argv[i] );
			}
		}
		else if ( !strcmp( argv[i], "-dlgshot" ) && i + 1 < argc )  // PREFIX: the settings window's pages, then exit
		{
			sdlg_snapshot( argv[++i] );
			sdlg_open( NULL, NULL );
			return 0;
		}
		else if ( !strcmp( argv[i], "-smoke" ) && i + 1 < argc )  // FILE: the panel drawn once, then exit
		{
			// needs no firmware: the build workflow's check that the program starts and draws
			rc = gui_init( dir ) == 0 && gui_screenshot( argv[++i] ) == 0 ? 0 : 1;
			gui_shutdown();
			return rc;
		}
		else if ( !strcmp( argv[i], "-svg2bmp" ) && i + 3 < argc )  // IN OUT WIDTH, then exit
		{
			i += 3;
			if ( dw_svg2bmp( argv[i - 2], argv[i - 1], atoi( argv[i] ) ) != 0 )
			{
				fprintf( stderr, "-svg2bmp: cannot render %s\n", argv[i - 2] );
				return 1;
			}
			return 0;
		}
		else if ( !strcmp( argv[i], "-wlog" ) && i + 1 < argc )
		{
			sscanf( argv[++i], "%x-%x", &mc_wlog_lo, &mc_wlog_hi );
			mc_wlog_n = 200000;
		}
		else if ( !strcmp( argv[i], "-k" ) && i + 1 < argc )  // T:G.I[+HOLD],... key press; T:wN wheel
		{
			const char *s = argv[++i];
			dw_key_event e;
			double t, hold;
			int g, x, n, m, j;

			while ( dw_nkeys < 254 )
			{
				if ( sscanf( s, "%lf:%d.%d%n", &t, &g, &x, &n ) == 3 )
				{
					hold = 0.15;
					if ( s[n] == '+' && sscanf( s + n + 1, "%lf%n", &hold, &m ) == 1 )
					{
						n += m + 1;
					}
					dw_keys[dw_nkeys++] = ( dw_key_event ){ (uint64_t)( t * DW_CPU_HZ ), g, x, 1 };
					dw_keys[dw_nkeys++] = ( dw_key_event ){ (uint64_t)( ( t + hold ) * DW_CPU_HZ ), g, x, 0 };
				}
				else if ( sscanf( s, "%lf:w%d%n", &t, &x, &n ) == 2 )
				{
					dw_keys[dw_nkeys++] = ( dw_key_event ){ (uint64_t)( t * DW_CPU_HZ ), -1, x, 0 };
				}
				else
				{
					break;
				}
				s += n;
				if ( *s != ',' )
				{
					break;
				}
				s++;
			}
			// a held key's release can come after later presses: keep the list in time order (stable)
			for ( m = 1; m < dw_nkeys; m++ )
			{
				e = dw_keys[m];
				for ( j = m; j > 0 && dw_keys[j - 1].at > e.at; j-- )
				{
					dw_keys[j] = dw_keys[j - 1];
				}
				dw_keys[j] = e;
			}
		}
		else if ( !strcmp( argv[i], "-guishot" ) && i + 1 < argc )
		{
			dw_guishot = argv[++i];
		}
		else if ( !strcmp( argv[i], "-snap" ) && i + 1 < argc )
		{
			dw_snap = atof( argv[++i] );
		}
		else if ( !strcmp( argv[i], "-lcd" ) && i + 1 < argc )
		{
			dw_lcd_out = argv[++i];
		}
		else if ( !strcmp( argv[i], "-ram" ) && i + 1 < argc )
		{
			dw_ram_dump = argv[++i];
		}
		else if ( !strcmp( argv[i], "-it" ) && i + 1 < argc )
		{
			dw_input_t = atof( argv[++i] );
		}
		else if ( !strcmp( argv[i], "-perf" ) )
		{
			dw_perf = 1;
		}
		else if ( !strcmp( argv[i], "-fastpanel" ) )
		{
			dw_fast_panel = 1;
		}
		else if ( !strcmp( argv[i], "-fwswitch" ) && i + 1 < argc )  // SEC:ZIPNAME
		{
			double t = 0;
			int at = 0;

			if ( sscanf( argv[++i], "%lf:%n", &t, &at ) == 1 && at > 0 )
			{
				dw_fw_at = (uint64_t)( t * DW_CPU_HZ ) + 1;
				snprintf( dw_fw_test, sizeof dw_fw_test, "%s", argv[i] + at );
			}
		}
		else if ( !strcmp( argv[i], "-io" ) )
		{
			dw_log_io = 1;
		}
		else if ( !strcmp( argv[i], "-ioall" ) && i + 1 < argc )
		{
			dw_trace_asic = (uint64_t)( atof( argv[++i] ) * DW_CPU_HZ ) + 1;
		}
		else if ( !strcmp( argv[i], "-t" ) && i + 1 < argc )
		{
			dw_trace = 1;
			sscanf( argv[++i], "%x-%x", &dw_trace_lo, &dw_trace_hi );
		}
		else if ( !strcmp( argv[i], "-bp" ) && i + 1 < argc && dw_nbps < DW_MAX_BPS )
		{
			dw_bps[dw_nbps++] = (uint32_t)strtoul( argv[++i], NULL, 16 );
		}
		else if ( argv[i][0] == '-' )
		{
			dw_usage();
		}
		else
		{
			fwarg = argv[i];
		}
	}

	if ( !dw_log_fp )  // a double-clicked window has no console: log next to the program
	{
		snprintf( up, sizeof up, "%sEmu_Darwin.log", dir );
		dw_log_fp = fopen( up, "w" );
	}
	for ( i = 0; i < 2 * 8; i++ )  // disks from the settings, where the command line set none
	{
		if ( set_cfg.disk[i / 8][i % 8][0] && !sd_present( i / 8, i % 8 ) &&
			 sd_attach( i / 8, i % 8, set_cfg.disk[i / 8][i % 8] ) != 0 )
		{
			dw_log( "settings: disk image %s cannot be opened\n", set_cfg.disk[i / 8][i % 8] );
		}
	}
	if ( fwarg )
	{
		snprintf( fwpath, sizeof fwpath, "%s", fwarg );
	}
	else
	{
		// the one the settings name (settings window, Options), else the newest; the folders
		// reach the project's Firmware folder wherever the build lands
		rc = dw_find_firmware( set_cfg.firmware, fwpath, sizeof fwpath );
		if ( rc != 0 && set_cfg.firmware[0] )
		{
			dw_log( "firmware %s not found: running the newest found\n", set_cfg.firmware );
			rc = dw_find_firmware( "", fwpath, sizeof fwpath );
		}
		if ( rc != 0 )
		{
			snprintf( err, sizeof err, "No firmware found: put emu_darwin_v2_50.zip next to the program\n"
					  "(or in a Firmware folder beside it or up to three folders above),\n"
					  "or name it on the command line.\n\nProgram folder: %.300s", dir );
			gui_error( err );
			return 1;
		}
	}
	rc = dw_load_firmware( fwpath, err, sizeof err );  // and its own settings memory
	if ( rc != 0 )
	{
		char msg[768];

		snprintf( msg, sizeof msg, "Firmware %s rejected (%d):\n%s", fwpath, rc, err );
		gui_error( msg );
		return 1;
	}

	mc_dbg_duart.tx = dw_dbg_tx;
	mc_duart.tx = dw_cpu_tx;
	mc_duart.ch[0].fast = set_cfg.fast_panel || dw_fast_panel;  // channel A: the panel link
	mc_power_on();
	dw_limit = seconds > 0 ? (uint64_t)( seconds * DW_CPU_HZ ) : UINT64_MAX;

	if ( !headless && gui_init( dir ) != 0 )
	{
		dw_log( "no window (%s): running headless\n", SDL_GetError() );
		headless = 1;
		dw_turbo = 1;
	}
	if ( headless )
	{
		dw_run( NULL );
	}
	else
	{
		midi_open( set_cfg.midi_in, set_cfg.midi_out );
		if ( !dw_turbo && au_open() != 0 )
		{
			dw_log( "audio: no playback device, running silent\n" );
		}
		dw_mtx = SDL_CreateMutex();
		th = SDL_CreateThread( dw_run, "emu", NULL );
		perf_t0 = SDL_GetPerformanceCounter();
		perf_emu0 = dw_cycles;
		while ( !dw_done )
		{
			if ( gui_frame() != 0 )
			{
				dw_quit = 1;
				break;
			}
			frames++;
			if ( dw_perf && SDL_GetPerformanceCounter() - perf_t0 >= SDL_GetPerformanceFrequency() )
			{
				// once a wall second: emulated time, frame rate, lock waits, MIDI out and its time code
				double f = (double)SDL_GetPerformanceFrequency(), wall = ( SDL_GetPerformanceCounter() - perf_t0 ) / f;

				dw_log( "perf: emu %.3f s per wall s, gui %.0f fps, lock wait %.1f ms (max %.1f), MIDI %u bytes, MTC %u qf,"
						" SCSI %u commands, MIDI LED %u flashes\n",
						(double)( dw_cycles - perf_emu0 ) / DW_CPU_HZ / wall, frames / wall, dw_perf_wait * 1000 / f,
						dw_perf_wait_max * 1000 / f, dw_perf_midi, dw_perf_mtc, wd_activity() - perf_scsi0,
						sim_port_falls( 0, 1 ) - perf_led0 );
				perf_scsi0 = wd_activity();
				perf_led0 = sim_port_falls( 0, 1 );
				perf_t0 = SDL_GetPerformanceCounter();
				perf_emu0 = dw_cycles;
				frames = 0;
				dw_perf_wait = dw_perf_wait_max = 0;
				dw_perf_midi = dw_perf_mtc = 0;
			}
			if ( dw_fw_req )  // the settings window chose another firmware
			{
				dw_fw_req = 0;
				dw_switch_firmware();
			}
			if ( dw_power_req )
			{
				dw_power_req = 0;
				dw_lock();
				if ( dw_powered )
				{
					dw_powered = 0;
					rtc_save();
					au_close();
				}
				else
				{
					mc_power_on();
					dw_powered = 1;
					if ( !dw_turbo )
					{
						au_open();  // the devices chosen in the settings since
					}
				}
				dw_unlock();
			}
		}
		SDL_WaitThread( th, NULL );
		midi_close();
		au_close();
	}
	au_wav_close();
	rtc_save();
	mc_unmapped_report();
	dw_dump_profile();
	v50_state( err, sizeof err );
	dw_log( "V50: %s\n", err );
	asic_v50_state( err, sizeof err );
	dw_log( "V50 interrupts: %s\n", err );
	if ( dw_lcd_out )
	{
		uint8_t bm[PNL_ROWB * PNL_H];

		pnl_bitmap( bm );
		dw_write_lcd( dw_lcd_out, bm );
	}
	if ( dw_ram_dump )
	{
		FILE *f = fopen( dw_ram_dump, "wb" );

		if ( f )
		{
			fwrite( dw_ram, 1, DW_RAM_SIZE, f );
			fwrite( v50_mem, 1, V50_MEM_SIZE, f );  // the V50's megabyte follows
			fclose( f );
		}
	}
	if ( !headless )
	{
		if ( dw_guishot )
		{
			gui_screenshot( dw_guishot );
		}
		gui_shutdown();
	}
	return 0;
}
