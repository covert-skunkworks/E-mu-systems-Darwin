//  Description:
//
//		rtc.h -- DS12887-class real-time clock with battery-backed NVRAM.
//
//		The Darwin keeps all of its user settings in the clock chip's 114
//		bytes of NVRAM (registers 0E-7F). The firmware addresses them as
//		NVRAM bytes 00-71 (ROM 051312 / 051354 add 0E); byte 70 is the
//		layout version (7) and byte 71 the checksum, the low byte of the sum
//		of bytes 00-70 (ROM 047E38).
//
//		Only that NVRAM section is persisted, as a 114-byte file next to the
//		program; the clock itself follows the host's time, with any setting
//		made in the firmware kept as an offset for the session.
//
//		Bus interface (CS3): D00002 selects a register, D00000 is the data,
//		both as 16-bit words carrying the byte in D7-D0.
//
//		  reg  0-9  time and date (BCD or binary, see reg B)
//		  reg  A    UIP, oscillator, rate select (periodic interrupt)
//		  reg  B    SET PIE AIE UIE SQWE DM 24/12 DSE
//		  reg  C    IRQF PF AF UF (cleared when read)
//		  reg  D    VRT (battery good)
//

#ifndef RTC_H
#define RTC_H

#include <stdint.h>

#define RTC_NV_FIRST 0x0E  // first NVRAM register
#define RTC_NV_SIZE  114   // registers 0E-7F

/**
  * @brief  Power-on: load the NVRAM section from its file and set the clock.
  *
  * A missing or short file leaves the NVRAM zeroed; the firmware then finds
  * a bad checksum and restores its defaults, as on a unit with a flat cell.
  *
  * @param  nvram_path The settings file, NULL for none (nothing is saved).
  *
  * @retval 0 loaded, 1 no file (blank NVRAM), -1 the file is unreadable.
  */
int rtc_init( const char *nvram_path );

/**
  * @brief  Select the register the next data access goes to (D00002).
  *
  * @param  reg Register number, 0-127.
  *
  * @retval None.
  */
void rtc_select( uint8_t reg );

/**
  * @brief  Read the selected register (D00000).
  *
  * @retval The register value.
  */
uint8_t rtc_read( void );

/**
  * @brief  Write the selected register (D00000).
  *
  * @param  v The value.
  *
  * @retval None.
  */
void rtc_write( uint8_t v );

/**
  * @brief  Advance the clock's interrupt timers.
  *
  * @param  cycles CPU cycles elapsed.
  * @param  cpu_hz CPU clock, Hz.
  *
  * @retval None.
  */
void rtc_tick( int cycles, double cpu_hz );

/**
  * @brief  Is the clock's IRQ output asserted?
  *
  * @retval 1 if asserted, 0 if not.
  */
int rtc_irq( void );

/**
  * @brief  Write the NVRAM section to its file if it changed since the last save.
  *
  * @retval 0 saved or nothing to save, -1 the file could not be written.
  */
int rtc_save( void );

/**
  * @brief  Copy out the NVRAM section (settings window, tests).
  *
  * @param  out Receives RTC_NV_SIZE bytes.
  *
  * @retval None.
  */
void rtc_nvram( uint8_t out[RTC_NV_SIZE] );

#endif
