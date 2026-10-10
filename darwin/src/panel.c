//  Description:
//
//		panel.c -- the Darwin's front panel, emulated at its link protocol.
//
//		Two halves:
//
//		  The link. Bytes from the 68306 are framed into messages (each runs
//		  from one F0 to the next, or to a pause on the line); config, LED
//		  and meter messages update panel state here, graphics messages go to
//		  the panel's processor. Keys and the wheel go back as messages into
//		  the DUART's receiver.
//
//		  The panel's processor. A second Musashi context with its own 2 MB
//		  of RAM runs the firmware's graphics library on a local screen. For
//		  each graphics message it calls the ROM's decoder (085892) and
//		  answers the decoder's "next value" calls (ROM 01DB56) from the
//		  message; the decoder's replies (01DAF2, 01DB1C, 01DB4E) are dropped,
//		  the 68306 does not wait for them. Anything that would reach the
//		  kernel or the serial ports ends the call.
//
//		  Panel memory map (only while its context runs):
//		    000000-0FFFFF  the firmware ROM (shared, read-only)
//		    100000-1FFFFF  panel RAM: the graphics library's variables at
//		                   their link-time addresses (as in the 68306's RAM)
//		    200000-2FFFFF  the panel's own: fonts from 200000, the screen
//		                   descriptor at 2EFF00, the display at 2F0000, the
//		                   stack below 2FF000, the return trap at 2FFFF0
//		                   (above the 68306's RAM, so no release's variables
//		                   can collide with them)
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "panel.h"
#include "darwin.h"
#include "machine.h"

#include <stdlib.h>
#include <string.h>

#include "m68k.h"

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define PNL_RAM_BASE  0x100000
#define PNL_RAM_SIZE  0x200000
#define PNL_FONTS     0x200000  // downloaded fonts, bump-allocated
#define PNL_FONTS_END 0x2EFF00
#define PNL_DESC      0x2EFF00  // screen descriptor for GrNewScreen
#define PNL_FB        0x2F0000  // the 240 x 64 bitmap
#define PNL_STACK     0x2FF000
#define PNL_MAGIC     0x2FFFF0  // "bra.s *": a call returns here

// firmware routines the panel uses, found by signature at power-on (pnl_locate), so any
// release works; the v2.5 addresses in brackets
#define PNL_FN_NEWSCREEN pnl_rom.newscreen  // GrNewScreen( flag.w, desc.l, driver.l ) [084502]
#define PNL_FN_DECODE    pnl_rom.decode     // decode and execute one graphics command [085892]
#define PNL_FN_NEWFONT   pnl_rom.newfont    // register a font( kind.w, data.l ) [08767E]
#define PNL_TRAP_VALUE   pnl_rom.value      // read the next 14-bit value from the link [01DB56]
#define PNL_TRAP_SEND0   pnl_rom.send0      // reply: start [01DAF2]
#define PNL_TRAP_SEND1   pnl_rom.send1      // reply: a value [01DB1C]
#define PNL_TRAP_SEND2   pnl_rom.send2      // reply: end [01DB4E = send1 + 32]
#define PNL_FONT_TABLE   pnl_rom.font_table // registered fonts: 10 x ( kind.w, data.l ) [1BBC28]
#define PNL_FONT_TABLE_LEN 60

#define PNL_BUDGET    20000000     // cycles one call may take before it is abandoned
#define PNL_FIFO      ( 1 << 18 )
#define PNL_MSG_MAX   ( 1 << 17 )
#define PNL_IDLE      20000        // 68306 cycles of silence that end a message (~1.2 ms)
#define PNL_DEVICE    0x00         // the panel's device number on the link

//------------------------------------------------------------------------------
//---------------------- Local types and structures ----------------------------
//------------------------------------------------------------------------------

typedef struct
{
	uint32_t newscreen, decode, newfont, value, send0, send1, send2, font_table;
	int      found;
} pnl_layout;

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

// v2.5 code as signatures; ".." = a masked byte (absolute addresses that move between releases)
static const struct
{
	const char *name;
	const char *hex;
} pnl_sigs[6] = {
	{ "newscreen", "4E56........000E2F2E000A4240102E00093F004EB9........4E5E" },
	{ "decode", "4E56FC0C48E73F3047F9........4E9324407E0DBE8A670004926D5A7E06BE8A6700029C6D2A7E03" },
	{ "value", "4E56........2F0245F9........4E9234004E9248C0EF8048C280820800" },
	{ "send0", "4E56........242E00084EB9........3F3C00014EB9........2F024EB9" },
	{ "send1", "4E56........2F02242E000830020240007F3F0045F9........4E92EE82" },
	{ "newfont", "4E56........322E0008242E000A424043F9........41F9........4A90" },
};

static pnl_layout pnl_rom;

static uint8_t   pnl_ram[PNL_RAM_SIZE];
static uint8_t  *pnl_ctx_main, *pnl_ctx_panel;
static int       pnl_in;              // the panel context is running
static int       pnl_abort;           // the running call hit something it must not do
static int       pnl_ready;           // the panel's screen exists
static int       pnl_skip_root;       // the 68306's first "new bitmap" is the root we already have
static uint32_t  pnl_font_next;

static uint8_t   pnl_fifo[PNL_FIFO];  // link bytes from the 68306
static unsigned  pnl_fr, pnl_fw;
static int       pnl_idle;

static int32_t   pnl_vals[PNL_MSG_MAX / 2];  // values of the graphics message being executed
static int       pnl_nvals, pnl_vi;
static uint8_t   pnl_msg[PNL_MSG_MAX];

static uint8_t   pnl_leds[8][16];
static uint8_t   pnl_groups[8][8];
static int       pnl_contrast_v = 8;
static unsigned  pnl_logged;

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static uint32_t pnl_find( const char *hex );
static int pnl_locate( void );
static void pnl_send( const uint8_t *b, int n );
static uint32_t pnl_rd32( uint32_t a );
static void pnl_wr16( uint32_t a, uint16_t v );
static void pnl_wr32( uint32_t a, uint32_t v );
static int pnl_call( uint32_t fn, int n, const uint32_t *val, const int *size, uint32_t *ret );
static void pnl_engine_init( void );
static void pnl_graphics( const uint8_t *p, int n );
static void pnl_message( const uint8_t *m, int n );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Find the first place in the ROM that matches a signature.
  *
  * @param  hex Two hex digits per byte, ".." for a byte that may differ.
  *
  * @retval The address, 0 if there is no match.
  */
static uint32_t pnl_find( const char *hex )
{
	uint8_t pat[64], mask[64];
	uint32_t a;
	int n = 0, i;
	unsigned v;

	for ( ; hex[0] && hex[1] && n < 64; hex += 2, n++ )
	{
		mask[n] = hex[0] != '.';
		pat[n] = 0;
		if ( mask[n] && sscanf( hex, "%2x", &v ) == 1 )
		{
			pat[n] = (uint8_t)v;
		}
	}
	for ( a = 0; a + (uint32_t)n <= DW_ROM_SIZE; a += 2 )  // code is word aligned
	{
		for ( i = 0; i < n; i++ )
		{
			if ( mask[i] && dw_rom[a + (uint32_t)i] != pat[i] )
			{
				break;
			}
		}
		if ( i == n )
		{
			return a;
		}
	}
	return 0;
}

/**
  * @brief  Locate the firmware routines the panel runs (any release).
  *
  * @retval 0 all found, -1 something is missing (the display stays dark).
  */
static int pnl_locate( void )
{
	uint32_t *dst[6] = { &pnl_rom.newscreen, &pnl_rom.decode, &pnl_rom.value, &pnl_rom.send0,
						 &pnl_rom.send1, &pnl_rom.newfont };
	int k;

	memset( &pnl_rom, 0, sizeof pnl_rom );
	for ( k = 0; k < 6; k++ )
	{
		*dst[k] = pnl_find( pnl_sigs[k].hex );
		if ( !*dst[k] )
		{
			dw_log( "panel: firmware routine '%s' not found: no display\n", pnl_sigs[k].name );
			return -1;
		}
	}
	pnl_rom.send2 = pnl_rom.send1 + 0x32;  // the empty "end" routine right after it
	// the font table is the first LEA operand of the font registration (43F9 xxxxxxxx at +10)
	pnl_rom.font_table = ( (uint32_t)dw_rom[pnl_rom.newfont + 0x12] << 24 ) |
						 ( (uint32_t)dw_rom[pnl_rom.newfont + 0x13] << 16 ) |
						 ( (uint32_t)dw_rom[pnl_rom.newfont + 0x14] << 8 ) | dw_rom[pnl_rom.newfont + 0x15];
	if ( pnl_rom.font_table - DW_RAM_BASE >= DW_RAM_SIZE - PNL_FONT_TABLE_LEN )
	{
		dw_log( "panel: font table address %06X is not in RAM: no display\n", (unsigned)pnl_rom.font_table );
		return -1;
	}
	pnl_rom.found = 1;
	dw_log( "panel: firmware routines: screen %06X, decoder %06X, link %06X, fonts %06X\n",
			(unsigned)pnl_rom.newscreen, (unsigned)pnl_rom.decode, (unsigned)pnl_rom.value,
			(unsigned)pnl_rom.font_table );
	return 0;
}

/**
  * @brief  Send bytes to the 68306 (into DUART channel A's receiver).
  *
  * @param  b The bytes.
  * @param  n How many.
  *
  * @retval None.
  */
static void pnl_send( const uint8_t *b, int n )
{
	int i;

	for ( i = 0; i < n; i++ )
	{
		duart_rx_push( &mc_duart, 0, b[i] );
	}
}

/**
  * @brief  Read a big-endian long from panel RAM.
  *
  * @param  a Address in the panel's RAM range.
  *
  * @retval The long.
  */
static uint32_t pnl_rd32( uint32_t a )
{
	uint8_t *p = pnl_ram + ( ( a - PNL_RAM_BASE ) & ( PNL_RAM_SIZE - 1 ) );

	return ( (uint32_t)p[0] << 24 ) | ( (uint32_t)p[1] << 16 ) | ( (uint32_t)p[2] << 8 ) | p[3];
}

/**
  * @brief  Write a big-endian word to panel RAM.
  *
  * @param  a Address in the panel's RAM range.
  * @param  v The word.
  *
  * @retval None.
  */
static void pnl_wr16( uint32_t a, uint16_t v )
{
	uint8_t *p = pnl_ram + ( ( a - PNL_RAM_BASE ) & ( PNL_RAM_SIZE - 1 ) );

	p[0] = (uint8_t)( v >> 8 );
	p[1] = (uint8_t)v;
}

/**
  * @brief  Write a big-endian long to panel RAM.
  *
  * @param  a Address in the panel's RAM range.
  * @param  v The long.
  *
  * @retval None.
  */
static void pnl_wr32( uint32_t a, uint32_t v )
{
	pnl_wr16( a, (uint16_t)( v >> 16 ) );
	pnl_wr16( a + 2, (uint16_t)v );
}

/**
  * @brief  Call a ROM function on the panel's processor.
  *
  * The arguments are pushed C-style (last first) with the sizes the
  * function reads them at; it returns to a trap that ends the run.
  *
  * @param  fn Function address.
  * @param  n Number of arguments.
  * @param  val Argument values.
  * @param  size Argument sizes, 2 or 4 bytes.
  * @param  ret Receives D0, may be NULL.
  *
  * @retval 0 returned normally, -1 abandoned (budget or a forbidden call).
  */
static int pnl_call( uint32_t fn, int n, const uint32_t *val, const int *size, uint32_t *ret )
{
	uint32_t sp = PNL_STACK, d0;
	int i, budget = PNL_BUDGET, ok;

	m68k_get_context( pnl_ctx_main );
	m68k_set_context( pnl_ctx_panel );
	pnl_in = 1;
	pnl_abort = 0;
	for ( i = n - 1; i >= 0; i-- )
	{
		sp -= (uint32_t)size[i];
		if ( size[i] == 2 )
		{
			pnl_wr16( sp, (uint16_t)val[i] );
		}
		else
		{
			pnl_wr32( sp, val[i] );
		}
	}
	sp -= 4;
	pnl_wr32( sp, PNL_MAGIC );
	m68k_set_reg( M68K_REG_SR, 0x2700 );  // supervisor, interrupts masked
	m68k_set_reg( M68K_REG_A7, sp );
	m68k_set_reg( M68K_REG_PC, fn );
	while ( m68k_get_reg( NULL, M68K_REG_PC ) != PNL_MAGIC && !pnl_abort && budget > 0 )
	{
		budget -= m68k_execute( 20000 );
	}
	ok = m68k_get_reg( NULL, M68K_REG_PC ) == PNL_MAGIC && !pnl_abort;
	d0 = m68k_get_reg( NULL, M68K_REG_D0 );
	if ( !ok && pnl_logged++ < 20 )
	{
		dw_log( "panel: call %06X abandoned at %06X (%s)\n", (unsigned)fn,
				m68k_get_reg( NULL, M68K_REG_PC ), budget <= 0 ? "too long" : "forbidden call" );
	}
	m68k_get_context( pnl_ctx_panel );
	pnl_in = 0;
	m68k_set_context( pnl_ctx_main );
	if ( ret )
	{
		*ret = d0;
	}
	return ok ? 0 : -1;
}

/**
  * @brief  Start the panel's processor: clear its RAM, create its local screen.
  *
  * @retval None.
  */
static void pnl_engine_init( void )
{
	static const int sizes[3] = { 2, 4, 4 };
	uint32_t args[3], id = 0;

	memset( pnl_ram, 0, sizeof pnl_ram );
	pnl_wr16( PNL_MAGIC, 0x60FE );  // bra.s *
	pnl_wr16( PNL_DESC + 0, PNL_W );
	pnl_wr16( PNL_DESC + 2, PNL_H );
	pnl_ram[PNL_DESC + 4 - PNL_RAM_BASE] = 1;  // bits per pixel
	pnl_ram[PNL_DESC + 5 - PNL_RAM_BASE] = 0;
	pnl_wr16( PNL_DESC + 6, PNL_ROWB );
	pnl_wr32( PNL_DESC + 8, PNL_FB );
	pnl_font_next = PNL_FONTS;

	// a local screen (flag 1): the library draws straight into PNL_FB
	args[0] = 1;
	args[1] = PNL_DESC;
	args[2] = 0;
	pnl_ready = pnl_call( PNL_FN_NEWSCREEN, 3, args, sizes, &id ) == 0;
	pnl_skip_root = 1;
	dw_log( "panel: local screen %s (id %u)\n", pnl_ready ? "created" : "FAILED", (unsigned)id );
}

/**
  * @brief  Execute one graphics message.
  *
  * @param  p The message bytes after the "01" kind byte: 7-bit pairs.
  * @param  n Their number.
  *
  * @retval None.
  */
static void pnl_graphics( const uint8_t *p, int n )
{
	static const int font_sizes[2] = { 2, 4 };
	uint32_t args[2];
	int i, cmd, len;

	pnl_nvals = 0;
	for ( i = 0; i + 1 < n; i += 2 )  // 14 bits, low 7 first; bit 13 is the sign (ROM 01DB56)
	{
		int32_t v = p[i] | ( p[i + 1] << 7 );

		pnl_vals[pnl_nvals++] = ( v & 0x2000 ) ? v - 0x4000 : v;
	}
	if ( pnl_nvals == 0 || !pnl_ready )
	{
		return;
	}
	cmd = pnl_vals[0];
	if ( cmd == 0x14 )  // font download: id, length, bytes
	{
		len = pnl_nvals - 3;
		if ( len <= 0 || pnl_font_next + (uint32_t)len > PNL_FONTS_END )
		{
			dw_log( "panel: font %d (%d bytes) does not fit\n", pnl_vals[1], len );
			return;
		}
		for ( i = 0; i < len; i++ )
		{
			pnl_ram[pnl_font_next + (uint32_t)i - PNL_RAM_BASE] = (uint8_t)pnl_vals[3 + i];
		}
		args[0] = (uint32_t)pnl_vals[1];
		args[1] = pnl_font_next;
		pnl_call( PNL_FN_NEWFONT, 2, args, font_sizes, NULL );
		pnl_font_next = ( pnl_font_next + (uint32_t)len + 15 ) & ~15u;
		return;
	}
	if ( cmd == 0x11 && pnl_nvals >= 3 && pnl_vals[2] == 0 && pnl_skip_root )
	{
		pnl_skip_root = 0;  // the root bitmap of our screen exists already
		return;
	}
	// the real panel has the same fonts built in, registered in the same order as the 68306
	// registers them (ROM 01D9F0): a copy of the 68306's font table gives the same ids
	memcpy( pnl_ram + ( PNL_FONT_TABLE - PNL_RAM_BASE ), dw_ram + ( PNL_FONT_TABLE - DW_RAM_BASE ),
			PNL_FONT_TABLE_LEN );
	pnl_vi = 0;
	i = pnl_call( PNL_FN_DECODE, 0, NULL, NULL, NULL );
	if ( ( pnl_vi != pnl_nvals || i != 0 ) && dw_log_io )
	{
		dw_log( "panel: command %02X used %d of %d values%s\n", cmd, pnl_vi, pnl_nvals,
				i ? ", abandoned" : "" );
	}
}

/**
  * @brief  Handle one complete message from the 68306.
  *
  * @param  m The message, from its F0.
  * @param  n Its length.
  *
  * @retval None.
  */
static void pnl_message( const uint8_t *m, int n )
{
	static const uint8_t version[5] = { '1', '.', '0', '0', ' ' };
	uint8_t reply[12];

	if ( n < 5 || m[0] != 0xF0 || m[1] != 0x18 || m[2] != 0x0B )
	{
		return;
	}
	switch ( m[4] )
	{
	case 0x00:  // configuration
		if ( n >= 7 && m[5] == 0x01 )
		{
			pnl_contrast_v = m[6] & 15;
		}
		else if ( n >= 6 && m[5] == 0x08 )  // version request
		{
			reply[0] = 0xF0;
			reply[1] = 0x18;
			reply[2] = 0x0B;
			reply[3] = m[3];
			reply[4] = 0x00;
			reply[5] = 0x09;
			memcpy( reply + 6, version, 5 );
			pnl_send( reply, 11 );
		}
		break;
	case 0x01:
		pnl_graphics( m + 5, n - 5 );
		break;
	case 0x02:  // LED
		if ( n >= 8 )
		{
			pnl_leds[m[5] & 7][m[6] & 15] = m[7];  // 0 off, 1 on, 2 and up flashing
			if ( dw_trace_asic && dw_cycles >= dw_trace_asic )
			{
				dw_log( "%10.6f panel LED %d.%d = %d\n", (double)dw_cycles / DW_CPU_HZ, m[5], m[6], m[7] );
			}
		}
		break;
	case 0x03:
		if ( n >= 14 )
		{
			memcpy( pnl_groups[m[5] & 7], m + 6, 8 );
			if ( dw_trace_asic && dw_cycles >= dw_trace_asic )
			{
				dw_log( "%10.6f panel group %d: %02X %02X %02X %02X %02X %02X %02X %02X\n", (double)dw_cycles / DW_CPU_HZ,
						m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13] );
			}
		}
		break;
	default:
		if ( pnl_logged++ < 20 )
		{
			dw_log( "panel: message kind %02X (%d bytes) ignored\n", m[4], n );
		}
		break;
	}
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Another firmware was loaded: find its routines again at the next power-on.
  *
  * @retval None.
  */
void pnl_rom_changed( void )
{
	memset( &pnl_rom, 0, sizeof pnl_rom );
}

/**
  * @brief  Power-on: clear the link and the display, start the panel's processor.
  *
  * @retval None.
  */
void pnl_power_on( void )
{
	if ( !pnl_ctx_main )
	{
		pnl_ctx_main = malloc( m68k_context_size() );
		pnl_ctx_panel = malloc( m68k_context_size() );
	}
	m68k_get_context( pnl_ctx_panel );  // a copy of the freshly reset CPU
	pnl_fr = pnl_fw = 0;
	pnl_idle = 0;
	memset( pnl_leds, 0, sizeof pnl_leds );
	memset( pnl_groups, 0, sizeof pnl_groups );
	pnl_contrast_v = 8;
	pnl_logged = 0;
	pnl_ready = 0;
	if ( pnl_rom.found || pnl_locate() == 0 )
	{
		pnl_engine_init();
	}
}

/**
  * @brief  A byte the 68306 sends on the panel link (DUART channel A).
  *
  * @param  b The byte.
  *
  * @retval None.
  */
void pnl_rx( uint8_t b )
{
	if ( ( pnl_fw + 1 ) % PNL_FIFO == pnl_fr )
	{
		return;  // full: the panel would have lost it too
	}
	pnl_fifo[pnl_fw] = b;
	pnl_fw = ( pnl_fw + 1 ) % PNL_FIFO;
	pnl_idle = 0;
}

/**
  * @brief  Process queued link traffic (outside the 68306's execution).
  *
  * @param  cycles 68306 cycles since the last call.
  *
  * @retval None.
  */
void pnl_poll( int cycles )
{
	unsigned p, end;
	int n;

	pnl_idle += cycles;
	while ( pnl_fr != pnl_fw )
	{
		if ( pnl_fifo[pnl_fr] != 0xF0 )  // stray byte between messages
		{
			pnl_fr = ( pnl_fr + 1 ) % PNL_FIFO;
			continue;
		}
		// the message ends at the next F0, or at the end of the queue after a pause
		end = PNL_FIFO;
		for ( p = ( pnl_fr + 1 ) % PNL_FIFO; p != pnl_fw; p = ( p + 1 ) % PNL_FIFO )
		{
			if ( pnl_fifo[p] == 0xF0 )
			{
				end = p;
				break;
			}
		}
		if ( end == PNL_FIFO )
		{
			if ( pnl_idle < PNL_IDLE )
			{
				return;
			}
			end = pnl_fw;
		}
		for ( n = 0, p = pnl_fr; p != end && n < PNL_MSG_MAX; p = ( p + 1 ) % PNL_FIFO )
		{
			pnl_msg[n++] = pnl_fifo[p];
		}
		pnl_fr = end;
		pnl_message( pnl_msg, n );
	}
}

/**
  * @brief  Copy the display bitmap.
  *
  * @param  out Receives PNL_ROWB * PNL_H bytes, 1 = dark pixel.
  *
  * @retval None.
  */
void pnl_bitmap( uint8_t *out )
{
	memcpy( out, pnl_ram + ( PNL_FB - PNL_RAM_BASE ), PNL_ROWB * PNL_H );
}

/**
  * @brief  LED state.
  *
  * @param  group LED group 0-7.
  * @param  index LED index 0-15.
  * @retval 0 dark, 1 lit, 2 and up flashing (4 an armed track, 2 / 5 during record start).
  * @retval 1 lit, 0 dark.
  */
int pnl_led( int group, int index )
{
	return pnl_leds[group & 7][index & 15];
}

/**
  * @brief  The LCD contrast the firmware set (0-15, 8 = middle).
  *
  * @retval Contrast.
  */
int pnl_contrast( void )
{
	return pnl_contrast_v;
}

/**
  * @brief  The 8 bytes last sent for a "03" group (meters).
  *
  * @param  group Group number.
  * @param  out Receives 8 bytes.
  *
  * @retval None.
  */
void pnl_group_bytes( int group, uint8_t out[8] )
{
	memcpy( out, pnl_groups[group & 7], 8 );
}

/**
  * @brief  A panel key changed.
  *
  * @param  group Key group 0-6.
  * @param  index Key index in the group.
  * @param  down 1 pressed, 0 released.
  *
  * @retval None.
  */
void pnl_key( int group, int index, int down )
{
	uint8_t m[8] = { 0xF0, 0x18, 0x0B, PNL_DEVICE, 0x04, 0, 0, 0 };

	m[5] = (uint8_t)( group & 0x7F );
	m[6] = (uint8_t)( index & 0x7F );
	m[7] = (uint8_t)( down ? 1 : 0 );
	pnl_send( m, 8 );
}

/**
  * @brief  The data wheel turned.
  *
  * @param  steps Steps, positive = clockwise.
  *
  * @retval None.
  */
void pnl_wheel( int steps )
{
	static int pos;
	uint8_t m[9] = { 0xF0, 0x18, 0x0B, PNL_DEVICE, 0x05, 0, 0, 0, 0 };

	if ( steps > 63 )
	{
		steps = 63;
	}
	if ( steps < -64 )
	{
		steps = -64;
	}
	pos = ( pos + steps ) & 0x3FFF;
	m[6] = (uint8_t)( steps & 0x7F );  // signed 7-bit delta (ROM 00AA7C)
	m[7] = (uint8_t)( pos & 0x7F );
	m[8] = (uint8_t)( pos >> 7 );
	pnl_send( m, 9 );
}

/**
  * @brief  Is the panel's processor in the middle of a call (memory routing)?
  *
  * @retval 1 while the panel context runs.
  */
int pnl_active( void )
{
	return pnl_in;
}

/**
  * @brief  Instruction hook while the panel context runs.
  *
  * @param  pc The instruction's address.
  *
  * @retval None.
  */
void pnl_hook( uint32_t pc )
{
	uint32_t sp;
	int32_t v;

	if ( pc == PNL_MAGIC )
	{
		m68k_end_timeslice();
		return;
	}
	if ( pc == PNL_TRAP_VALUE )  // D0 = the next value of the message
	{
		v = pnl_vi < pnl_nvals ? pnl_vals[pnl_vi] : 0;
		pnl_vi++;
		m68k_set_reg( M68K_REG_D0, (unsigned)v );
	}
	else if ( pc != PNL_TRAP_SEND0 && pc != PNL_TRAP_SEND1 && pc != PNL_TRAP_SEND2 )  // replies: dropped
	{
		if ( pc == 0x000340 || pc == 0x000350 )  // the kernel's task switch / sleep: never on the panel
		{
			pnl_abort = 1;
			m68k_set_reg( M68K_REG_PC, PNL_MAGIC );
			m68k_end_timeslice();
		}
		return;
	}
	sp = m68k_get_reg( NULL, M68K_REG_A7 );  // return from the trapped function
	m68k_set_reg( M68K_REG_PC, pnl_rd32( sp ) );
	m68k_set_reg( M68K_REG_A7, sp + 4 );
}

/**
  * @brief  Panel memory read (while pnl_active()).
  *
  * @param  a Address (24-bit).
  *
  * @retval The byte.
  */
uint8_t pnl_read8( uint32_t a )
{
	if ( a < DW_ROM_SIZE )
	{
		return dw_rom[a];
	}
	if ( a - PNL_RAM_BASE < PNL_RAM_SIZE )
	{
		return pnl_ram[a - PNL_RAM_BASE];
	}
	return 0xFF;
}

/**
  * @brief  Panel memory write (while pnl_active()).
  *
  * @param  a Address (24-bit).
  * @param  v The byte.
  *
  * @retval None.
  */
void pnl_write8( uint32_t a, uint8_t v )
{
	if ( a - PNL_RAM_BASE < PNL_RAM_SIZE )
	{
		pnl_ram[a - PNL_RAM_BASE] = v;
	}
}
