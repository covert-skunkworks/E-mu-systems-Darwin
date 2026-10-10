//  Description:
//
//		wd33c93.h -- Western Digital WD33C93A SCSI bus interface controller.
//
//		The disk engine has two ("3393" in its debug monitor): chip 0 on the
//		host bus at V50 I/O 2000, chip 1 on the audio bus at 2010. Each has
//		an address register and a data register:
//
//		  base + 0   write: register number; read: auxiliary status
//		  base + 2   the selected register (auto-increment, except at 18-1F)
//		  base + 4   board control latch (written A5, then 81 at init)
//		  base + 8   written A5 at init (purpose unknown)
//
//		  reg  00 own ID / CDB size   01 control (DMA mode in bits 7-5)
//		       02 timeout period      03-0E CDB bytes 1-12
//		       0F target LUN / status 10 command phase
//		       11 synchronous xfer    12-14 transfer count (MSB first)
//		       15 destination ID      16 source ID
//		       17 SCSI status         18 command        19 data
//		  aux  7 INT  6 LCI  5 BSY  4 CIP  1 PE  0 DBR
//
//		Data phases in DMA mode go through the audio ASIC's DMA channel for
//		the chip (asic.h); in polled mode through register 19.
//

#ifndef WD33C93_H
#define WD33C93_H

#include <stdint.h>

#define WD_CHIPS 2

/**
  * @brief  Power-on reset of both chips.
  *
  * @retval None.
  */
void wd_power_on( void );

/**
  * @brief  Read a chip port.
  *
  * @param  chip 0 or 1.
  * @param  off Offset from the chip's base (0, 2, 4, 8).
  *
  * @retval The byte.
  */
uint8_t wd_read( int chip, int off );

/**
  * @brief  Write a chip port.
  *
  * @param  chip 0 or 1.
  * @param  off Offset from the chip's base (0, 2, 4, 8).
  * @param  v The byte.
  *
  * @retval None.
  */
void wd_write( int chip, int off, uint8_t v );

/**
  * @brief  Advance both chips (selection, disk and transfer delays).
  *
  * @param  cycles V50 cycles elapsed.
  *
  * @retval None.
  */
void wd_tick( int cycles );

/**
  * @brief  Bus activity: SCSI commands started so far (both buses).
  *
  * @retval A count that moves with every selection.
  */
unsigned wd_activity( void );

/**
  * @brief  Is a chip's interrupt asserted?
  *
  * @param  chip 0 or 1.
  *
  * @retval 1 if asserted.
  */
int wd_irq( int chip );

#endif
