//  Description:
//
//		duart.h -- MC68681 dual UART (the 68306's on-chip one and the
//		optional debug DUART at B00000).
//
//		Registers sit on odd byte addresses, register n at base + 2n + 1.
//
//		  n   read                 write
//		  0   MR1A/MR2A            MR1A/MR2A
//		  1   SRA                  CSRA
//		  2   -                    CRA
//		  3   RHRA                 THRA
//		  4   IPCR                 ACR
//		  5   ISR                  IMR
//		  6   CUR                  CTUR
//		  7   CLR                  CTLR
//		  8-B as 0-3 for channel B
//		  C   IVR                  IVR
//		  D   IP (input pins)      OPCR
//		  E   start counter        set OP bits
//		  F   stop counter         reset OP bits
//
//		  SR   0 RxRDY  1 FFULL  2 TxRDY  3 TxEMT  4 overrun
//		  ISR  0 TxRDYA 1 RxRDYA 3 counter 4 TxRDYB 5 RxRDYB 7 IP change
//
//		Characters are paced at the programmed baud rate, so the firmware
//		sees the timing a real link has; the host side pushes received bytes
//		into a queue and gets transmitted bytes through a callback.
//

#ifndef DUART_H
#define DUART_H

#include <stdint.h>

#define DUART_RXQ 4096  // host-side receive queue per channel

typedef void ( *duart_tx_fn )( int chan, uint8_t byte, void *ctx );

typedef struct
{
	uint8_t  mr1, mr2;
	int      mr_ptr;           // 0 = MR1 next, 1 = MR2
	uint8_t  csr;
	int      rx_on, tx_on;
	uint8_t  fifo[3];          // receive FIFO
	int      fifo_n;
	uint8_t  status;           // error bits 4-7
	int      tx_busy;          // cycles until the transmitter is free
	uint8_t  tx_byte;
	uint8_t  rxq[DUART_RXQ];   // bytes waiting to "arrive"
	int      rxq_r, rxq_w;
	int      rx_wait;          // cycles until the next byte may arrive
	int      fast;             // 1: characters take 1/16 of their time (not like the hardware)
} duart_chan;

typedef struct
{
	const char *name;
	double      xtal;          // X1 clock, Hz
	double      ip2_hz;        // clock on IP2 (counter/timer source), Hz
	double      cpu_hz;
	duart_chan  ch[2];
	uint8_t     acr, imr, ivr, opcr, opr, ip;
	uint8_t     ctur, ctlr;
	duart_tx_fn tx;
	void       *tx_ctx;
} duart;

/**
  * @brief  Reset a DUART (power-on state); keeps name, clocks and callback.
  *
  * @param  d The DUART.
  *
  * @retval None.
  */
void duart_reset( duart *d );

/**
  * @brief  Read register n (0-15).
  *
  * @param  d The DUART.
  * @param  n Register number.
  *
  * @retval The register value.
  */
uint8_t duart_read( duart *d, int n );

/**
  * @brief  Write register n (0-15).
  *
  * @param  d The DUART.
  * @param  n Register number.
  * @param  v The value.
  *
  * @retval None.
  */
void duart_write( duart *d, int n, uint8_t v );

/**
  * @brief  Advance the DUART by a number of CPU cycles.
  *
  * @param  d The DUART.
  * @param  cycles CPU cycles.
  *
  * @retval None.
  */
void duart_tick( duart *d, int cycles );

/**
  * @brief  Is the interrupt output asserted (ISR & IMR)?
  *
  * @param  d The DUART.
  *
  * @retval 1 if asserted, 0 if not.
  */
int duart_irq( const duart *d );

/**
  * @brief  Queue a byte arriving on a channel's receive line.
  *
  * @param  d The DUART.
  * @param  chan 0 = A, 1 = B.
  * @param  b The byte.
  *
  * @retval 0 queued, -1 queue full.
  */
int duart_rx_push( duart *d, int chan, uint8_t b );

/**
  * @brief  Free room in a channel's receive queue.
  *
  * @param  d The DUART.
  * @param  chan 0 = A, 1 = B.
  *
  * @retval Bytes that can still be queued.
  */
int duart_rx_space( const duart *d, int chan );

#endif
