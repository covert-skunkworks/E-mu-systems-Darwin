//  Description:
//
//		darwin.h -- shared declarations of the E-mu Darwin emulator.
//
//		The Darwin (1997) is an 8-track hard-disk recorder. Its host CPU is a
//		Motorola MC68306 (68000 core with DRAM controller, chip selects and a
//		68681 DUART on chip), running E-mu's firmware from two EPROMs. Disk
//		I/O and audio streaming are done by a separate "Audio Disk
//		Controller" (ADC): an NEC V30 whose 64 KB program the 68306 uploads
//		from its own ROM at boot.
//
//		Memory map (24-bit; found by tracing the firmware, docs/hardware.md):
//
//		  000000-0FFFFF  EPROM, 1 MB (2 x 27C040, MSB = even bytes)
//		  100000-1FFFFF  DRAM, 1 MB (stack top 1F7FFE)
//		  B00000-B0001F  optional debug DUART (68681, odd bytes; probed with
//		                 a bus-error trap; channel B = debug console)
//		  C00000-C001FF  E-mu audio ASIC: 8 track channels, meters, IPC FIFO
//		                 to the V30 (C6 read, C8 write), data wheel (C2)
//		  D00000/D00002  RTC (DS12887 class): data / register select;
//		                 114 bytes of battery-backed NVRAM hold the settings
//		  E00000-E0FFFF  DSP card window (optional, probed)
//		  F00000-F0FFFF  option cards: SMPTE (F01E00), PLL / ADAT (F01F00)
//		  FFF7E0-FFF7FF  68306 DUART: A = remote link, B = MIDI
//		  FFFFC0-FFFFFF  68306 system registers
//
//		Interrupts: L1 RTC periodic tick (autovector), L2 PLL/ADAT card, L3
//		audio ASIC, L4 68306 DUART (vector from its IVR, 65), L5 SMPTE card,
//		L6 debug DUART (vector 64).
//

#ifndef DARWIN_H
#define DARWIN_H

#include <stdint.h>
#include <stdio.h>

#define DW_CPU_HZ   16000000  // 68306 clock (16 MHz part; exact crystal unconfirmed)
#define DW_ROM_SIZE 0x100000
#define DW_RAM_BASE 0x100000
#define DW_RAM_SIZE 0x100000

extern uint8_t  dw_rom[DW_ROM_SIZE];
extern uint8_t  dw_ram[DW_RAM_SIZE];
#ifndef DW_VERSION
#define DW_VERSION "1.0"  // the emulator's version (the About tab); a release build sets it from its tag
#endif

// 1 = title the windows "Covert Skunkworks" instead of "E-mu" (the Makefile sets it: make COVERT=1)
#ifndef DW_COVERT
#define DW_COVERT 0
#endif
#if DW_COVERT
#define DW_TITLE "Covert Skunkworks Darwin"
#else
#define DW_TITLE "E-mu Darwin"
#endif

extern char     dw_fw_version[64];  // the loaded firmware's version string
extern uint64_t dw_cycles;        // emulated CPU cycles since power-on
extern int      dw_log_io;        // -io: log every unmapped access
extern uint64_t dw_trace_asic;    // -ioall: log every ASIC access from this cycle on (0 = off)

/**
  * @brief  printf to the log file (or stderr).
  *
  * @param  fmt printf format.
  *
  * @retval None.
  */
void dw_log( const char *fmt, ... );

/**
  * @brief  The PC of the instruction being executed.
  *
  * @retval The PC.
  */
uint32_t dw_pc( void );

/**
  * @brief  Recompute the CPU's interrupt level from all sources.
  *
  * @retval None.
  */
void dw_irq_update( void );

extern volatile int dw_powered;  // 0: switched off at the panel

/**
  * @brief  Take the emulator lock (GUI thread, around input and drawing).
  *
  * @retval None.
  */
void dw_lock( void );

/**
  * @brief  Release the emulator lock.
  *
  * @retval None.
  */
void dw_unlock( void );

/**
  * @brief  The panel's power button was pressed.
  *
  * @retval None.
  */
void dw_power_request( void );

#define DW_FW_NAME 64  // room for a firmware zip's file name

extern char dw_fw_zip[DW_FW_NAME];  // the running firmware's file name

/**
  * @brief  The firmware zips the program can find (emu_darwin_v*.zip next to it, in a
  *         Firmware folder beside it, and the same up to three folders above).
  *
  * @param  names Receives the file names, sorted, each found once.
  * @param  max Room in names.
  *
  * @retval The number found.
  */
int dw_firmware_list( char names[][DW_FW_NAME], int max );

/**
  * @brief  The firmware setting changed: restart the unit with it (as a power cycle).
  *
  * Each firmware keeps its own settings memory, darwin_nvram_<version>.bin.
  *
  * @retval None.
  */
void dw_firmware_request( void );

#endif
