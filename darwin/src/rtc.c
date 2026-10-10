//  Description:
//
//		rtc.c -- DS12887-class real-time clock with battery-backed NVRAM.
//
//		Time registers are computed from the host clock when read, so the
//		emulated unit always knows the date. When the firmware sets the
//		clock (SET bit in register B), the new time is kept as an offset
//		from the host clock.
//
//		Interrupts: the periodic interrupt drives the firmware's system tick
//		(level 1, ROM 000CBC reads register C to acknowledge it); the
//		update-ended flag is raised once per second for completeness.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "rtc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define RTC_A 0x0A
#define RTC_B 0x0B
#define RTC_C 0x0C
#define RTC_D 0x0D

#define RTC_B_SET 0x80
#define RTC_B_PIE 0x40
#define RTC_B_AIE 0x20
#define RTC_B_UIE 0x10
#define RTC_B_DM  0x04  // 1 = binary, 0 = BCD
#define RTC_B_24  0x02

#define RTC_C_IRQF 0x80
#define RTC_C_PF   0x40
#define RTC_C_UF   0x10

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static uint8_t     rtc_reg[128];
static uint8_t     rtc_sel;
static long        rtc_offset;      // emulated clock - host clock, seconds
static double      rtc_pi_acc;      // CPU cycles toward the next periodic interrupt
static double      rtc_uf_acc;      // ... toward the next update-ended flag
static const char *rtc_path;
static int         rtc_dirty;
static int         rtc_year_base = 1980;  // the Darwin counts the year register from 1980 (ROM 051716)

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static uint8_t rtc_enc( int v );
static int rtc_dec( uint8_t v );
static void rtc_now( struct tm *out );
static uint8_t rtc_time_reg( int reg );
static void rtc_set_from_regs( void );
static double rtc_period( void );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Encode a time field in the format register B selects.
  *
  * @param  v The value, 0-99.
  *
  * @retval BCD or binary.
  */
static uint8_t rtc_enc( int v )
{
	if ( rtc_reg[RTC_B] & RTC_B_DM )
	{
		return (uint8_t)v;
	}
	return (uint8_t)( ( ( v / 10 ) << 4 ) | ( v % 10 ) );
}

/**
  * @brief  Decode a time field written in the format register B selects.
  *
  * @param  v BCD or binary.
  *
  * @retval The value.
  */
static int rtc_dec( uint8_t v )
{
	if ( rtc_reg[RTC_B] & RTC_B_DM )
	{
		return v;
	}
	return ( v >> 4 ) * 10 + ( v & 15 );
}

/**
  * @brief  The emulated wall-clock time.
  *
  * @param  out Receives the local time.
  *
  * @retval None.
  */
static void rtc_now( struct tm *out )
{
	time_t t = time( NULL ) + rtc_offset;
	struct tm *lt = localtime( &t );

	if ( lt )
	{
		*out = *lt;
	}
	else
	{
		memset( out, 0, sizeof *out );
	}
}

/**
  * @brief  Value of a time or date register (0-9).
  *
  * @param  reg Register number.
  *
  * @retval The register value.
  */
static uint8_t rtc_time_reg( int reg )
{
	struct tm tm;
	int h;

	if ( rtc_reg[RTC_B] & RTC_B_SET )  // being set: show what the firmware wrote
	{
		return rtc_reg[reg];
	}
	rtc_now( &tm );
	switch ( reg )
	{
	case 0:
		return rtc_enc( tm.tm_sec > 59 ? 59 : tm.tm_sec );
	case 2:
		return rtc_enc( tm.tm_min );
	case 4:
		if ( rtc_reg[RTC_B] & RTC_B_24 )
		{
			return rtc_enc( tm.tm_hour );
		}
		h = tm.tm_hour % 12;
		return (uint8_t)( rtc_enc( h ? h : 12 ) | ( tm.tm_hour >= 12 ? 0x80 : 0 ) );
	case 6:
		return rtc_enc( tm.tm_wday + 1 );
	case 7:
		return rtc_enc( tm.tm_mday );
	case 8:
		return rtc_enc( tm.tm_mon + 1 );
	case 9:
		return rtc_enc( ( tm.tm_year + 1900 - rtc_year_base ) % 100 );
	default:  // alarm registers
		return rtc_reg[reg];
	}
}

/**
  * @brief  The firmware has set the clock: keep its time as an offset.
  *
  * The year register counts from rtc_year_base (1980 on the Darwin: the
  * firmware adds 7BC when it reads the clock, ROM 051716).
  *
  * @retval None.
  */
static void rtc_set_from_regs( void )
{
	struct tm tm;
	time_t t;
	int h;

	memset( &tm, 0, sizeof tm );
	tm.tm_sec = rtc_dec( rtc_reg[0] );
	tm.tm_min = rtc_dec( rtc_reg[2] );
	if ( rtc_reg[RTC_B] & RTC_B_24 )
	{
		tm.tm_hour = rtc_dec( rtc_reg[4] );
	}
	else
	{
		h = rtc_dec( rtc_reg[4] & 0x7F ) % 12;
		tm.tm_hour = h + ( rtc_reg[4] & 0x80 ? 12 : 0 );
	}
	tm.tm_mday = rtc_dec( rtc_reg[7] );
	tm.tm_mon = rtc_dec( rtc_reg[8] ) - 1;
	tm.tm_year = rtc_dec( rtc_reg[9] ) + rtc_year_base - 1900;
	tm.tm_isdst = -1;
	t = mktime( &tm );
	if ( t != (time_t)-1 )
	{
		rtc_offset = (long)( t - time( NULL ) );
	}
}

/**
  * @brief  Periodic-interrupt period from register A's rate select.
  *
  * @retval Period in seconds, 0 if the periodic interrupt is off.
  */
static double rtc_period( void )
{
	int rs = rtc_reg[RTC_A] & 15;

	if ( rs == 0 )
	{
		return 0;
	}
	if ( rs <= 2 )  // the 32 kHz time base repeats rates 8 and 9 here
	{
		rs += 7;
	}
	return (double)( 1 << ( rs - 1 ) ) / 32768.0;
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Power-on: load the NVRAM section from its file and set the clock.
  *
  * @param  nvram_path The settings file, NULL for none (nothing is saved).
  *
  * @retval 0 loaded, 1 no file (blank NVRAM), -1 the file is unreadable.
  */
int rtc_init( const char *nvram_path )
{
	FILE *f;
	size_t n;

	memset( rtc_reg, 0, sizeof rtc_reg );
	rtc_reg[RTC_A] = 0x20;  // oscillator running
	// binary, 24-hour: how the Darwin keeps its clock (the battery holds this on a real unit; the
	// firmware reads the time registers as binary and never switches the mode at boot)
	rtc_reg[RTC_B] = RTC_B_DM | RTC_B_24;
	rtc_reg[RTC_D] = 0x80;  // VRT: the battery is good
	rtc_sel = 0;
	rtc_offset = 0;
	rtc_pi_acc = rtc_uf_acc = 0;
	rtc_path = nvram_path;
	rtc_dirty = 0;
	if ( !nvram_path )
	{
		return 1;
	}
	f = fopen( nvram_path, "rb" );
	if ( !f )
	{
		return 1;
	}
	n = fread( rtc_reg + RTC_NV_FIRST, 1, RTC_NV_SIZE, f );
	fclose( f );
	return n == RTC_NV_SIZE ? 0 : -1;
}

/**
  * @brief  Select the register the next data access goes to (D00002).
  *
  * @param  reg Register number, 0-127.
  *
  * @retval None.
  */
void rtc_select( uint8_t reg )
{
	rtc_sel = reg & 0x7F;
}

/**
  * @brief  Read the selected register (D00000).
  *
  * @retval The register value.
  */
uint8_t rtc_read( void )
{
	uint8_t v;

	if ( rtc_sel < RTC_A )
	{
		return rtc_time_reg( rtc_sel );
	}
	if ( rtc_sel == RTC_A )
	{
		return rtc_reg[RTC_A] & 0x7F;  // UIP never seen set: updates are instant
	}
	if ( rtc_sel == RTC_C )  // reading C acknowledges every flag
	{
		v = rtc_reg[RTC_C];
		rtc_reg[RTC_C] = 0;
		return v;
	}
	return rtc_reg[rtc_sel];
}

/**
  * @brief  Write the selected register (D00000).
  *
  * @param  v The value.
  *
  * @retval None.
  */
void rtc_write( uint8_t v )
{
	if ( rtc_sel == RTC_C || rtc_sel == RTC_D )  // read-only
	{
		return;
	}
	if ( rtc_sel == RTC_B )
	{
		if ( ( rtc_reg[RTC_B] & RTC_B_SET ) && !( v & RTC_B_SET ) )
		{
			rtc_reg[RTC_B] = v;
			rtc_set_from_regs();
			return;
		}
		if ( !( rtc_reg[RTC_B] & RTC_B_SET ) && ( v & RTC_B_SET ) )
		{
			int r;  // freeze the current time into the registers to be edited

			for ( r = 0; r < RTC_A; r++ )
			{
				rtc_reg[r] = rtc_time_reg( r );
			}
		}
		rtc_reg[RTC_B] = v;
		return;
	}
	if ( rtc_sel >= RTC_NV_FIRST && rtc_reg[rtc_sel] != v )
	{
		rtc_dirty = 1;
	}
	rtc_reg[rtc_sel] = v;
}

/**
  * @brief  Advance the clock's interrupt timers.
  *
  * @param  cycles CPU cycles elapsed.
  * @param  cpu_hz CPU clock, Hz.
  *
  * @retval None.
  */
void rtc_tick( int cycles, double cpu_hz )
{
	double period = rtc_period() * cpu_hz;

	if ( period > 0 )
	{
		rtc_pi_acc += cycles;
		while ( rtc_pi_acc >= period )
		{
			rtc_pi_acc -= period;
			rtc_reg[RTC_C] |= RTC_C_PF;
		}
	}
	rtc_uf_acc += cycles;
	while ( rtc_uf_acc >= cpu_hz )
	{
		rtc_uf_acc -= cpu_hz;
		if ( !( rtc_reg[RTC_B] & RTC_B_SET ) )
		{
			rtc_reg[RTC_C] |= RTC_C_UF;
		}
	}
	if ( ( rtc_reg[RTC_C] & RTC_C_PF && rtc_reg[RTC_B] & RTC_B_PIE ) ||
		 ( rtc_reg[RTC_C] & RTC_C_UF && rtc_reg[RTC_B] & RTC_B_UIE ) )
	{
		rtc_reg[RTC_C] |= RTC_C_IRQF;
	}
}

/**
  * @brief  Is the clock's IRQ output asserted?
  *
  * @retval 1 if asserted, 0 if not.
  */
int rtc_irq( void )
{
	return ( rtc_reg[RTC_C] & RTC_C_IRQF ) != 0;
}

/**
  * @brief  Write the NVRAM section to its file if it changed since the last save.
  *
  * @retval 0 saved or nothing to save, -1 the file could not be written.
  */
int rtc_save( void )
{
	FILE *f;
	size_t n;

	if ( !rtc_dirty || !rtc_path )
	{
		return 0;
	}
	f = fopen( rtc_path, "wb" );
	if ( !f )
	{
		return -1;
	}
	n = fwrite( rtc_reg + RTC_NV_FIRST, 1, RTC_NV_SIZE, f );
	if ( fclose( f ) != 0 || n != RTC_NV_SIZE )
	{
		return -1;
	}
	rtc_dirty = 0;
	return 0;
}

/**
  * @brief  Copy out the NVRAM section (settings window, tests).
  *
  * @param  out Receives RTC_NV_SIZE bytes.
  *
  * @retval None.
  */
void rtc_nvram( uint8_t out[RTC_NV_SIZE] )
{
	memcpy( out, rtc_reg + RTC_NV_FIRST, RTC_NV_SIZE );
}
