//  Description:
//
//		v50.h -- the Darwin's disk engine: an NEC V50 with 1 MB of memory.
//
//		The 68306 uploads the engine's program into V50 memory F0000-FFFFF
//		through the audio ASIC and then lets it run (see asic.h). The V50
//		starts at FFFF:0000 like any 8086.
//
//		V50 I/O map (found in the program, docs/disk-engine.md):
//
//		  2000-200F  SCSI controller 0, host port (WD33C93 class)
//		  2010-201F  SCSI controller 1, audio port
//		  3000-31FF  the audio ASIC
//		  FFF0-FFFF  V50 on-chip system registers
//

#ifndef V50_H
#define V50_H

#include <stdint.h>

#define V50_MEM_SIZE 0x100000
#define V50_HZ       10000000  // V50 clock (10 MHz part assumed)

extern uint8_t v50_mem[V50_MEM_SIZE];

/**
  * @brief  Where the V50 is: CS:IP of the instruction being executed.
  *
  * @retval CS << 16 | IP.
  */
uint32_t v50_where( void );

/**
  * @brief  Power-on: clear memory, hold the CPU.
  *
  * @retval None.
  */
void v50_power_on( void );

/**
  * @brief  Run the V50 for the time the 68306 just ran.
  *
  * Starts the CPU from its reset vector when the ASIC releases it.
  *
  * @param  cycles_68k 68306 cycles elapsed.
  *
  * @retval None.
  */
void v50_run( int cycles_68k );

/**
  * @brief  One-line CPU state for logs.
  *
  * @param  out Receives the text.
  * @param  n Room in out.
  *
  * @retval None.
  */
void v50_state( char *out, int n );

#endif
