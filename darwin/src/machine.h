//  Description:
//
//		machine.h -- the Darwin main board: memory map, interrupts, devices.
//
//		Owns the CPU-facing side of every chip (see darwin.h for the map) and
//		the Musashi callbacks. Single-threaded: the caller serialises access
//		(the GUI thread takes the emulator lock).
//

#ifndef MACHINE_H
#define MACHINE_H

#include <stdint.h>

#include "duart.h"

extern duart mc_duart;      // 68306 on-chip DUART: A = remote link, B = MIDI
extern duart mc_dbg_duart;  // optional debug DUART at B00000
extern int   mc_dbg_fitted; // the debug DUART answers (else bus errors)
extern uint32_t mc_wlog_lo, mc_wlog_hi;  // -wlog: log byte writes made by code in this PC range
extern int   mc_wlog_n;              // ... this many more

/**
  * @brief  Cold start: reset every chip and the CPU (ROM and NVRAM are kept).
  *
  * @retval None.
  */
void mc_power_on( void );

/**
  * @brief  Run the CPU and the chips for a slice of emulated time.
  *
  * @param  cycles CPU cycles to run.
  *
  * @retval Cycles actually run.
  */
int mc_run( int cycles );

/**
  * @brief  Read a byte without side effects (debugger, disassembler).
  *
  * @param  a Address (24-bit).
  *
  * @retval The byte, FF for I/O.
  */
uint8_t mc_peek8( uint32_t a );

/**
  * @brief  Log a summary of the unmapped addresses the firmware touched.
  *
  * @retval None.
  */
void mc_unmapped_report( void );

#endif
