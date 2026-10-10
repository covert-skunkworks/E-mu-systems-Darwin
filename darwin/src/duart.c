//  Description:
//
//		duart.c -- MC68681 dual UART.
//
//		Enough of the chip for the Darwin firmware's interrupt-driven serial
//		drivers (ROM 000412 for the 68306 DUART, 00098E for the debug one):
//		receive FIFO, transmitter, status and interrupt registers, the
//		output port and the baud-rate generator (only to pace characters).
//		The counter/timer only serves as a baud clock; its interrupt is not
//		used by the firmware and is not modelled.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "duart.h"
#include "darwin.h"

#include <string.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define DUART_SR_RXRDY 0x01
#define DUART_SR_FFULL 0x02
#define DUART_SR_TXRDY 0x04
#define DUART_SR_TXEMT 0x08

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static double duart_baud( const duart *d, const duart_chan *c );
static int duart_char_cycles( const duart *d, const duart_chan *c );
static uint8_t duart_sr( const duart_chan *c );
static uint8_t duart_isr( const duart *d );
static void duart_command( duart_chan *c, uint8_t v );
static void duart_send( duart *d, int chan, uint8_t v );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  The receive / transmit rate a channel's CSR selects.
  *
  * The tables are the 68681's for a 3.6864 MHz crystal, scaled to the real
  * one: the Darwin runs its on-chip DUART from 3 MHz, which turns the
  * "38.4k" setting the firmware programs for MIDI into 31250 baud.
  *
  * @param  d The DUART.
  * @param  c The channel.
  *
  * @retval Baud rate (receiver setting; the firmware sets both alike).
  */
static double duart_baud( const duart *d, const duart_chan *c )
{
	static const double set1[13] = { 50, 110, 134.5, 200, 300, 600, 1200, 1050, 2400, 4800,
									 7200, 9600, 38400 };
	static const double set2[13] = { 75, 110, 134.5, 150, 300, 600, 1200, 2000, 2400, 4800,
									 1800, 9600, 19200 };
	int sel = c->csr >> 4, src, n;
	double f;

	if ( sel == 13 )  // the counter/timer's output, divided by 16
	{
		src = ( d->acr >> 4 ) & 7;
		f = src == 6 ? d->xtal : src == 7 ? d->xtal / 16 : src == 4 ? d->ip2_hz :
			src == 5 ? d->ip2_hz / 16 : d->xtal / 16;
		n = ( d->ctur << 8 ) | d->ctlr;
		if ( n == 0 )
		{
			n = 1;
		}
		return f / ( 2.0 * n ) / 16.0;
	}
	if ( sel > 13 )  // external 16x / 1x clock on IP pins
	{
		return 31250.0;
	}
	return ( d->acr & 0x80 ? set2 : set1 )[sel] * d->xtal / 3686400.0;
}

/**
  * @brief  CPU cycles one character (start, 8 data, stop) takes on a channel.
  *
  * @param  d The DUART.
  * @param  c The channel.
  *
  * @retval Cycles, at least 1.
  */
static int duart_char_cycles( const duart *d, const duart_chan *c )
{
	double baud = duart_baud( d, c );
	int n;

	if ( baud < 1 )
	{
		baud = 1;
	}
	n = (int)( d->cpu_hz * 10.0 / baud );
	n = c->fast ? n / 16 : n;  // the emulator's "fast panel link": same protocol, 16 times the rate
	return n > 0 ? n : 1;
}

/**
  * @brief  A channel's status register.
  *
  * @param  c The channel.
  *
  * @retval SR.
  */
static uint8_t duart_sr( const duart_chan *c )
{
	uint8_t sr = c->status & 0xF0;

	if ( c->fifo_n > 0 )
	{
		sr |= DUART_SR_RXRDY;
	}
	if ( c->fifo_n == 3 )
	{
		sr |= DUART_SR_FFULL;
	}
	if ( c->tx_on && c->tx_busy == 0 )
	{
		sr |= DUART_SR_TXRDY | DUART_SR_TXEMT;
	}
	return sr;
}

/**
  * @brief  The interrupt status register.
  *
  * @param  d The DUART.
  *
  * @retval ISR.
  */
static uint8_t duart_isr( const duart *d )
{
	uint8_t isr = 0, sr;
	int k;

	for ( k = 0; k < 2; k++ )
	{
		sr = duart_sr( &d->ch[k] );
		if ( sr & DUART_SR_TXRDY )
		{
			isr |= (uint8_t)( 0x01 << ( 4 * k ) );
		}
		// MR1 bit 6 selects FFULL instead of RxRDY as the receive interrupt
		if ( sr & ( d->ch[k].mr1 & 0x40 ? DUART_SR_FFULL : DUART_SR_RXRDY ) )
		{
			isr |= (uint8_t)( 0x02 << ( 4 * k ) );
		}
	}
	return isr;
}

/**
  * @brief  Carry out a write to a channel's command register.
  *
  * @param  c The channel.
  * @param  v The command byte.
  *
  * @retval None.
  */
static void duart_command( duart_chan *c, uint8_t v )
{
	switch ( ( v >> 4 ) & 7 )
	{
	case 1:  // reset MR pointer
		c->mr_ptr = 0;
		break;
	case 2:  // reset receiver
		c->rx_on = 0;
		c->fifo_n = 0;
		c->status &= 0x0F;
		break;
	case 3:  // reset transmitter
		c->tx_on = 0;
		c->tx_busy = 0;
		break;
	case 4:  // reset error status
		c->status &= 0x0F;
		break;
	default:  // break control: no line to model
		break;
	}
	if ( ( v & 3 ) == 1 )
	{
		c->rx_on = 1;
	}
	else if ( ( v & 3 ) == 2 )
	{
		c->rx_on = 0;
	}
	if ( ( ( v >> 2 ) & 3 ) == 1 )
	{
		c->tx_on = 1;
	}
	else if ( ( ( v >> 2 ) & 3 ) == 2 )
	{
		c->tx_on = 0;
	}
}

/**
  * @brief  The firmware wrote a transmit holding register.
  *
  * @param  d The DUART.
  * @param  chan 0 = A, 1 = B.
  * @param  v The byte.
  *
  * @retval None.
  */
static void duart_send( duart *d, int chan, uint8_t v )
{
	duart_chan *c = &d->ch[chan];

	if ( !c->tx_on )
	{
		return;
	}
	if ( c->tx_busy > 0 && d->tx )  // written while busy: the previous byte goes out now
	{
		d->tx( chan, c->tx_byte, d->tx_ctx );
	}
	c->tx_byte = v;
	c->tx_busy = duart_char_cycles( d, c );
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Reset a DUART (power-on state); keeps name, clocks and callback.
  *
  * @param  d The DUART.
  *
  * @retval None.
  */
void duart_reset( duart *d )
{
	int fast0 = d->ch[0].fast, fast1 = d->ch[1].fast;  // an emulator setting, not chip state

	memset( d->ch, 0, sizeof d->ch );
	d->ch[0].fast = fast0;
	d->ch[1].fast = fast1;
	d->acr = d->imr = d->opcr = d->opr = 0;
	d->ivr = 0x0F;  // the 68681's reset value
	d->ctur = d->ctlr = 0;
}

/**
  * @brief  Read register n (0-15).
  *
  * @param  d The DUART.
  * @param  n Register number.
  *
  * @retval The register value.
  */
uint8_t duart_read( duart *d, int n )
{
	duart_chan *c = &d->ch[( n >> 3 ) & 1];
	uint8_t v;
	int i;

	switch ( n & 15 )
	{
	case 0:
	case 8:
		v = c->mr_ptr ? c->mr2 : c->mr1;
		c->mr_ptr = 1;
		return v;
	case 1:
	case 9:
		return duart_sr( c );
	case 3:
	case 11:
		if ( c->fifo_n == 0 )
		{
			return c->fifo[0];
		}
		v = c->fifo[0];
		for ( i = 1; i < c->fifo_n; i++ )
		{
			c->fifo[i - 1] = c->fifo[i];
		}
		c->fifo_n--;
		return v;
	case 4:   // IPCR: no input changes modelled
		return 0x00;
	case 5:
		return duart_isr( d );
	case 6:
		return d->ctur;
	case 7:
		return d->ctlr;
	case 12:
		return d->ivr;
	case 13:
		return d->ip;
	default:  // 2, 10 reserved; 14 start counter, 15 stop counter
		return 0xFF;
	}
}

/**
  * @brief  Write register n (0-15).
  *
  * @param  d The DUART.
  * @param  n Register number.
  * @param  v The value.
  *
  * @retval None.
  */
void duart_write( duart *d, int n, uint8_t v )
{
	duart_chan *c = &d->ch[( n >> 3 ) & 1];

	switch ( n & 15 )
	{
	case 0:
	case 8:
		if ( c->mr_ptr )
		{
			c->mr2 = v;
		}
		else
		{
			c->mr1 = v;
		}
		c->mr_ptr = 1;
		break;
	case 1:
	case 9:
		c->csr = v;
		break;
	case 2:
	case 10:
		duart_command( c, v );
		break;
	case 3:
	case 11:
		duart_send( d, ( n >> 3 ) & 1, v );
		break;
	case 4:
		d->acr = v;
		break;
	case 5:
		d->imr = v;
		break;
	case 6:
		d->ctur = v;
		break;
	case 7:
		d->ctlr = v;
		break;
	case 12:
		d->ivr = v;
		break;
	case 13:
		d->opcr = v;
		break;
	case 14:
		d->opr |= v;
		break;
	case 15:
		d->opr &= (uint8_t)~v;
		break;
	}
}

/**
  * @brief  Advance the DUART by a number of CPU cycles.
  *
  * @param  d The DUART.
  * @param  cycles CPU cycles.
  *
  * @retval None.
  */
void duart_tick( duart *d, int cycles )
{
	duart_chan *c;
	int k;

	for ( k = 0; k < 2; k++ )
	{
		c = &d->ch[k];
		if ( c->tx_busy > 0 )
		{
			c->tx_busy -= cycles;
			if ( c->tx_busy <= 0 )
			{
				c->tx_busy = 0;
				if ( d->tx )
				{
					d->tx( k, c->tx_byte, d->tx_ctx );
				}
			}
		}
		if ( c->rx_wait > 0 )
		{
			c->rx_wait -= cycles;
		}
		if ( c->rx_wait <= 0 && c->rxq_r != c->rxq_w )
		{
			if ( !c->rx_on )  // a disabled receiver ignores the line
			{
				if ( dw_log_io || ( dw_trace_asic && dw_cycles >= dw_trace_asic ) )
				{
					dw_log( "%10.6f %s %c rx off: %d bytes dropped\n", (double)dw_cycles / DW_CPU_HZ, d->name,
							'A' + k, ( c->rxq_w - c->rxq_r + DUART_RXQ ) % DUART_RXQ );
				}
				c->rxq_r = c->rxq_w;
			}
			else if ( c->fifo_n < 3 )  // full FIFO: hold the byte (no overrun loss)
			{
				c->fifo[c->fifo_n++] = c->rxq[c->rxq_r];
				if ( dw_log_io || ( dw_trace_asic && dw_cycles >= dw_trace_asic ) )
				{
					dw_log( "%10.6f %s %c rx %02X\n", (double)dw_cycles / DW_CPU_HZ, d->name, 'A' + k,
							c->rxq[c->rxq_r] );
				}
				c->rxq_r = ( c->rxq_r + 1 ) % DUART_RXQ;
				c->rx_wait = duart_char_cycles( d, c );
			}
		}
	}
}

/**
  * @brief  Is the interrupt output asserted (ISR & IMR)?
  *
  * @param  d The DUART.
  *
  * @retval 1 if asserted, 0 if not.
  */
int duart_irq( const duart *d )
{
	return ( duart_isr( d ) & d->imr ) != 0;
}

/**
  * @brief  Queue a byte arriving on a channel's receive line.
  *
  * @param  d The DUART.
  * @param  chan 0 = A, 1 = B.
  * @param  b The byte.
  *
  * @retval 0 queued, -1 queue full.
  */
int duart_rx_push( duart *d, int chan, uint8_t b )
{
	duart_chan *c = &d->ch[chan & 1];
	int next = ( c->rxq_w + 1 ) % DUART_RXQ;

	if ( next == c->rxq_r )
	{
		return -1;
	}
	c->rxq[c->rxq_w] = b;
	c->rxq_w = next;
	return 0;
}

/**
  * @brief  Free room in a channel's receive queue.
  *
  * @param  d The DUART.
  * @param  chan 0 = A, 1 = B.
  *
  * @retval Bytes that can still be queued.
  */
int duart_rx_space( const duart *d, int chan )
{
	const duart_chan *c = &d->ch[chan & 1];

	return DUART_RXQ - 1 - ( c->rxq_w - c->rxq_r + DUART_RXQ ) % DUART_RXQ;
}
