//  Description:
//
//		wd33c93.c -- Western Digital WD33C93A SCSI bus interface controller.
//
//		Initiator role only, which is what the Darwin uses for its disks. The
//		combination commands (Select-and-Transfer, 08 / 09) are carried out
//		as a small state machine in emulated time:
//
//		  select -> command phase -> data phase -> status -> message in
//		  (phase register 10: 00, 30, 41/44, 50, 60), then SCSI status 16
//		  ("select-and-transfer complete") and INT, with the target's status
//		  byte in the target LUN register (0F).
//
//		A selection nobody answers ends with status 42 (timeout). Target-mode
//		commands (Wait-for-Select, 0C) are accepted and never complete, as no
//		host computer is connected.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "wd33c93.h"
#include "asic.h"
#include "darwin.h"
#include "scsidisk.h"

#include <string.h>

//------------------------------------------------------------------------------
//--------------------- Local macros and constants -----------------------------
//------------------------------------------------------------------------------

#define WD_OWN_ID   0x00
#define WD_CONTROL  0x01
#define WD_TLUN     0x0F
#define WD_PHASE    0x10
#define WD_TC_MSB   0x12
#define WD_DEST_ID  0x15
#define WD_STATUS   0x17
#define WD_COMMAND  0x18
#define WD_DATA     0x19

#define WD_AUX_INT  0x80
#define WD_AUX_LCI  0x40
#define WD_AUX_BSY  0x20
#define WD_AUX_CIP  0x10
#define WD_AUX_DBR  0x01

// delays in V50 cycles (10 MHz): selection, command overhead, per byte on the bus
#define WD_T_SELECT  500
#define WD_T_COMMAND 2000
#define WD_T_BYTE    2
#define WD_T_TIMEOUT 2500

enum { WD_IDLE = 0, WD_SELECTING, WD_COMMANDING, WD_DATA_PHASE, WD_STATUSING, WD_TIMEOUT, WD_WAIT_SEL };

//------------------------------------------------------------------------------
//---------------------- Local types and structures ----------------------------
//------------------------------------------------------------------------------

typedef struct
{
	uint8_t  reg[32];
	uint8_t  addr;
	uint8_t  latch4, latch8;  // board latches next to the chip
	int      intr, lci;
	int      state;
	int      delay;
	int      target;
	sd_xfer  x;
	uint32_t pos;             // polled data: bytes moved
	uint32_t want;            // bytes this data phase moves
} wd_chip;

//------------------------------------------------------------------------------
//---------------------------- Local variables ---------------------------------
//------------------------------------------------------------------------------

static wd_chip  wd[WD_CHIPS];
static unsigned wd_commands;  // selections started, both chips

//------------------------------------------------------------------------------
//----------------------- Local function prototypes ----------------------------
//------------------------------------------------------------------------------

static uint32_t wd_tc( const wd_chip *w );
static void wd_set_tc( wd_chip *w, uint32_t n );
static void wd_interrupt( wd_chip *w, uint8_t status );
static void wd_command( int chip, uint8_t cmd );
static void wd_step( int chip );

//------------------------------------------------------------------------------
//---------------------------- Local functions ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  The 24-bit transfer count.
  *
  * @param  w The chip.
  *
  * @retval The count.
  */
static uint32_t wd_tc( const wd_chip *w )
{
	return ( (uint32_t)w->reg[WD_TC_MSB] << 16 ) | ( (uint32_t)w->reg[WD_TC_MSB + 1] << 8 ) |
		   w->reg[WD_TC_MSB + 2];
}

/**
  * @brief  Set the 24-bit transfer count.
  *
  * @param  w The chip.
  * @param  n The count.
  *
  * @retval None.
  */
static void wd_set_tc( wd_chip *w, uint32_t n )
{
	w->reg[WD_TC_MSB] = (uint8_t)( n >> 16 );
	w->reg[WD_TC_MSB + 1] = (uint8_t)( n >> 8 );
	w->reg[WD_TC_MSB + 2] = (uint8_t)n;
}

/**
  * @brief  Finish a command: set the SCSI status register and raise INT.
  *
  * @param  w The chip.
  * @param  status SCSI status code.
  *
  * @retval None.
  */
static void wd_interrupt( wd_chip *w, uint8_t status )
{
	w->reg[WD_STATUS] = status;
	w->intr = 1;
	w->state = WD_IDLE;
	dw_irq_update();
}

/**
  * @brief  Start a command written to register 18.
  *
  * @param  chip 0 or 1.
  * @param  cmd The command byte.
  *
  * @retval None.
  */
static void wd_command( int chip, uint8_t cmd )
{
	wd_chip *w = &wd[chip];

	if ( dw_log_io )
	{
		dw_log( "wd%d: command %02X (dest %d, TC %06X)\n", chip, cmd, w->reg[WD_DEST_ID] & 7, wd_tc( w ) );
	}
	switch ( cmd & 0x7F )
	{
	case 0x00:  // reset
		w->state = WD_SELECTING;  // reuse the delay path, finishing below
		w->delay = WD_T_SELECT;
		w->target = -2;
		break;
	case 0x01:  // abort
	case 0x04:  // disconnect
		w->state = WD_IDLE;
		wd_interrupt( w, cmd == 0x01 ? 0x22 : 0x85 );
		break;
	case 0x02:  // assert ATN
	case 0x03:  // negate ACK
		break;
	case 0x08:  // select with ATN and transfer
	case 0x09:  // select without ATN and transfer
		w->target = w->reg[WD_DEST_ID] & 7;
		w->reg[WD_PHASE] = 0x00;
		w->state = WD_SELECTING;
		wd_commands++;  // bus activity (the panel's SCSI LED)
		w->delay = WD_T_SELECT;
		break;
	case 0x0C:  // wait for select and receive (target mode): nobody will select us
		w->state = WD_WAIT_SEL;
		break;
	default:
		dw_log( "wd%d: command %02X not emulated\n", chip, cmd );
		w->lci = 1;
		break;
	}
}

/**
  * @brief  Advance the running command to its next stage.
  *
  * @param  chip 0 or 1.
  *
  * @retval None.
  */
static void wd_step( int chip )
{
	wd_chip *w = &wd[chip];
	uint8_t cdb[16];
	uint32_t n;
	int i, len;

	switch ( w->state )
	{
	case WD_SELECTING:
		if ( w->target == -2 )  // reset completes: status 01 with advanced features enabled
		{
			// every register but Own ID is cleared; the V50 checks the timeout period (02) for 0
			memset( w->reg + 1, 0, WD_COMMAND - 1 );
			wd_interrupt( w, ( w->reg[WD_OWN_ID] & 0x08 ) ? 0x01 : 0x00 );
			return;
		}
		if ( !sd_present( chip, w->target ) )
		{
			w->state = WD_TIMEOUT;
			w->delay = WD_T_TIMEOUT;
			return;
		}
		w->reg[WD_PHASE] = 0x30;
		w->state = WD_COMMANDING;
		w->delay = WD_T_COMMAND;
		return;
	case WD_TIMEOUT:
		wd_interrupt( w, 0x42 );
		return;
	case WD_COMMANDING:
		len = sd_cdb_len( w->reg[3] );
		for ( i = 0; i < len; i++ )
		{
			cdb[i] = w->reg[3 + i];
		}
		for ( ; i < 16; i++ )
		{
			cdb[i] = 0;
		}
		sd_command( chip, w->target, cdb, &w->x );
		w->pos = 0;
		n = wd_tc( w );
		w->want = w->x.len < n ? w->x.len : n;
		if ( w->x.dir == SD_NONE || w->want == 0 )
		{
			w->state = WD_STATUSING;
			w->delay = WD_T_SELECT;
			return;
		}
		w->reg[WD_PHASE] = w->x.dir == SD_IN ? 0x41 : 0x44;
		w->state = WD_DATA_PHASE;
		w->delay = 0;
		return;
	case WD_DATA_PHASE:
		if ( ( w->reg[WD_CONTROL] >> 5 ) == 0 )  // polled: the V50 moves the bytes through reg 19
		{
			return;
		}
		if ( !asic_dma_ready( chip ) )  // DMA: wait until the engine's channel is started
		{
			return;
		}
		n = asic_dma_move( chip, w->x.data, w->want, w->x.dir == SD_IN );
		wd_set_tc( w, wd_tc( w ) - n );
		if ( w->x.dir == SD_OUT )
		{
			sd_finish( chip, w->target, &w->x );
		}
		w->state = WD_STATUSING;
		w->delay = (int)( n * WD_T_BYTE ) + WD_T_SELECT;
		return;
	case WD_STATUSING:
		w->reg[WD_TLUN] = w->x.status;
		w->reg[WD_PHASE] = 0x60;
		wd_interrupt( w, 0x16 );
		return;
	default:
		return;
	}
}

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Power-on reset of both chips.
  *
  * @retval None.
  */
void wd_power_on( void )
{
	memset( wd, 0, sizeof wd );
}

/**
  * @brief  Read a chip port.
  *
  * @param  chip 0 or 1.
  * @param  off Offset from the chip's base (0, 2, 4, 8).
  *
  * @retval The byte.
  */
uint8_t wd_read( int chip, int off )
{
	wd_chip *w = &wd[chip & 1];
	uint8_t v;

	if ( off == 0 )  // auxiliary status
	{
		v = (uint8_t)( ( w->intr ? WD_AUX_INT : 0 ) | ( w->lci ? WD_AUX_LCI : 0 ) );
		if ( w->state != WD_IDLE )
		{
			v |= WD_AUX_BSY;
		}
		if ( w->state == WD_DATA_PHASE && ( w->reg[WD_CONTROL] >> 5 ) == 0 )
		{
			v |= WD_AUX_DBR;
		}
		return v;
	}
	if ( off == 4 )
	{
		return w->latch4;
	}
	if ( off == 8 )
	{
		return w->latch8;
	}
	if ( off != 2 )
	{
		return 0xFF;
	}
	switch ( w->addr )
	{
	case WD_STATUS:  // reading the status acknowledges the interrupt
		v = w->reg[WD_STATUS];
		w->intr = 0;
		w->lci = 0;
		dw_irq_update();
		break;
	case WD_DATA:  // polled data in
		v = 0;
		if ( w->state == WD_DATA_PHASE && w->x.dir == SD_IN && w->pos < w->want )
		{
			v = w->x.data[w->pos++];
			wd_set_tc( w, wd_tc( w ) - 1 );
			if ( w->pos == w->want )
			{
				w->state = WD_STATUSING;
				w->delay = WD_T_SELECT;
			}
		}
		break;
	default:
		v = w->reg[w->addr];
		break;
	}
	if ( w->addr < WD_COMMAND )
	{
		w->addr = (uint8_t)( ( w->addr + 1 ) & 0x1F );
	}
	return v;
}

/**
  * @brief  Write a chip port.
  *
  * @param  chip 0 or 1.
  * @param  off Offset from the chip's base (0, 2, 4, 8).
  * @param  v The byte.
  *
  * @retval None.
  */
void wd_write( int chip, int off, uint8_t v )
{
	wd_chip *w = &wd[chip & 1];

	if ( off == 0 )
	{
		w->addr = v & 0x1F;
		return;
	}
	if ( off == 4 )
	{
		// bit 6 drives the chip's master reset (V50 program 003737 pulses it): on release the chip
		// comes up cleared and reports the reset like a RESET command
		if ( ( w->latch4 & 0x40 ) && !( v & 0x40 ) )
		{
			w->intr = 0;
			w->state = WD_SELECTING;
			w->delay = WD_T_SELECT;
			w->target = -2;
		}
		w->latch4 = v;
		return;
	}
	if ( off == 8 )
	{
		w->latch8 = v;
		return;
	}
	if ( off != 2 )
	{
		return;
	}
	if ( w->addr == WD_COMMAND )
	{
		wd_command( chip & 1, v );
		return;
	}
	if ( w->addr == WD_DATA )  // polled data out
	{
		if ( w->state == WD_DATA_PHASE && w->x.dir == SD_OUT && w->pos < w->want )
		{
			w->x.data[w->pos++] = v;
			wd_set_tc( w, wd_tc( w ) - 1 );
			if ( w->pos == w->want )
			{
				sd_finish( chip & 1, w->target, &w->x );
				w->state = WD_STATUSING;
				w->delay = WD_T_SELECT;
			}
		}
		return;
	}
	w->reg[w->addr] = v;
	if ( w->addr < WD_COMMAND )
	{
		w->addr = (uint8_t)( ( w->addr + 1 ) & 0x1F );
	}
}

/**
  * @brief  Advance both chips (selection, disk and transfer delays).
  *
  * @param  cycles V50 cycles elapsed.
  *
  * @retval None.
  */
void wd_tick( int cycles )
{
	int c;

	for ( c = 0; c < WD_CHIPS; c++ )
	{
		wd_chip *w = &wd[c];

		if ( w->state == WD_IDLE || w->state == WD_WAIT_SEL )
		{
			continue;
		}
		w->delay -= cycles;
		while ( w->delay <= 0 && w->state != WD_IDLE && w->state != WD_WAIT_SEL )
		{
			int before = w->state;

			wd_step( c );
			if ( w->state == before && w->state == WD_DATA_PHASE )  // waiting for DMA / the V50
			{
				w->delay = 0;
				break;
			}
		}
	}
}

/**
  * @brief  Bus activity: SCSI commands started so far (both buses).
  *
  * @retval A count that moves with every selection.
  */
unsigned wd_activity( void )
{
	return wd_commands;
}

/**
  * @brief  Is a chip's interrupt asserted?
  *
  * @param  chip 0 or 1.
  *
  * @retval 1 if asserted.
  */
int wd_irq( int chip )
{
	return wd[chip & 1].intr;
}
