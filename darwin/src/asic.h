//  Description:
//
//		asic.h -- E-mu audio ASIC, shared by the 68306 (C00000-C001FF) and
//		the V50 disk engine (I/O 3000-31FF).
//
//		The chip streams 8 tracks between the disk engine and the
//		converters, measures levels for the meters, passes command words
//		between the two processors and serves as the V50's interrupt
//		controller. Both processors see the same register file, offset for
//		offset. The control block C0-FF is each side's own, except the
//		transport registers C2 and EA, which both sides write. The 32-bit
//		registers (100-1BF) appear in each processor's own byte order: to
//		the 68306 the long at 194 is 194 = high word, 196 = low word, to the
//		V50 it is 3194 = low word, 3196 = high word. Registers as the
//		firmware uses them (16-bit unless noted):
//
//		  offset  68306 (C00000 + offset)          V50 (3000 + offset)
//		  00-3E   per-track setup, cleared at init
//		  40-9E   per-track blocks of 8 words
//		  A0-AE   meter peaks, tracks 1-8 (0-3F)
//		  BC-BE   track state, 4 bits per track     B8-BE (B8: record enables)
//		  C2      transport speed and direction, signed: 40 = 1x forward (shared)
//		  C6      IPC: read a word from the V50     write a word to the 68k
//		  C8      IPC: write a word to the V50      read a word from the 68k
//		  E0      interrupt causes: 2 counter wrap, 3 tick;
//		          write 1s to acknowledge
//		  E2      V50 memory data port (address 198, auto-increment)
//		  E4                                         event causes (bit n = track n),
//		                                             write back to ack
//		  EA      bit 0: hold the sample counter (0 = it runs; shared)
//		  EC      V50 control: 6 hold (upload), 3 read, 0 run
//		  F0                                         end of interrupt
//		  F4      IPC status / enables: bit 0 room to send, bit 1 a word is
//		          waiting, bit 2 enables the "room" interrupt (both sides)
//		  100-17C per-track longs (disk addresses)
//		  190     (long)                            compare: INT 12 when the counter gets there
//		  194     sample counter, 24 bits (long): the transport position;
//		          the 68306 writes it to locate
//		  198     V50 memory address for the E2 port (long)
//
//		Interrupts: 68306 level 3 (autovector); V50 vectored, INT 8 + n:
//		  INT 9  ASIC events (30E4)        INT 10 a command word from the 68k
//		  INT 11 room to send to the 68k   INT 12 the counter reached 3190
//		  INT 13 SCSI controller 1         INT 14 SCSI controller 0
//

#ifndef ASIC_H
#define ASIC_H

#include <stdint.h>

/**
  * @brief  Power-on reset.
  *
  * @param  v50_mem The V50's 1 MB memory (for the E2 / 198 port).
  *
  * @retval None.
  */
void asic_reset( uint8_t *v50_mem );

/**
  * @brief  68306 side: read a 16-bit register.
  *
  * @param  a Address, C00000-CFFFFF (even).
  *
  * @retval The word.
  */
uint16_t asic_read16( uint32_t a );

/**
  * @brief  68306 side: write a 16-bit register.
  *
  * @param  a Address, C00000-CFFFFF (even).
  * @param  v The word.
  *
  * @retval None.
  */
void asic_write16( uint32_t a, uint16_t v );

/**
  * @brief  V50 side: read a 16-bit register.
  *
  * @param  port I/O port, 3000-31FF.
  *
  * @retval The word.
  */
uint16_t asic_v50_in( uint16_t port );

/**
  * @brief  V50 side: write a 16-bit register.
  *
  * @param  port I/O port, 3000-31FF.
  * @param  v The word.
  *
  * @retval None.
  */
void asic_v50_out( uint16_t port, uint16_t v );

/**
  * @brief  Is the V50 interrupt line asserted?
  *
  * @retval 1 if asserted.
  */
int asic_v50_irq( void );

/**
  * @brief  One-line V50 interrupt state for logs.
  *
  * @param  out Receives the text.
  * @param  n Room in out.
  *
  * @retval None.
  */
void asic_v50_state( char *out, int n );

/**
  * @brief  V50 interrupt acknowledge.
  *
  * @retval The vector number.
  */
int asic_v50_ack( void );

/**
  * @brief  May the V50 run (not held for a program upload)?
  *
  * @retval 1 if it may run.
  */
int asic_v50_running( void );

/**
  * @brief  Has the V50 started the DMA channel of a SCSI controller?
  *
  * @param  chip 0 host bus, 1 audio bus.
  *
  * @retval 1 started.
  */
int asic_dma_ready( int chip );

/**
  * @brief  Move a SCSI data phase through a DMA channel.
  *
  * @param  chip 0 host bus, 1 audio bus.
  * @param  buf The SCSI side of the data.
  * @param  n Bytes the SCSI side offers or wants.
  * @param  to_mem 1 for data in (SCSI to memory), 0 for data out.
  *
  * @retval Bytes moved (at most the channel's count).
  */
uint32_t asic_dma_move( int chip, uint8_t *buf, uint32_t n, int to_mem );

/**
  * @brief  Advance the ASIC's timers.
  *
  * @param  cycles 68306 cycles elapsed.
  *
  * @retval None.
  */
void asic_tick( int cycles );

/**
  * @brief  Is the 68306 interrupt (level 3) asserted?
  *
  * @retval 1 if asserted, 0 if not.
  */
int asic_irq( void );

/**
  * @brief  The transport's sample counter.
  *
  * @retval 24-bit sample position.
  */
uint32_t asic_counter( void );

#endif
