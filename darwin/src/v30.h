//  Description:
//
//		v30.h -- NEC V30 / V50 CPU core (8086 instruction set with the
//		80186 additions and NEC's own extensions).
//
//		The Darwin's disk engine is an NEC V50 (uPD70216): a V30 core with
//		on-chip interrupt, timer, DMA and serial units (v50.c). This file is
//		only the core; memory is a flat 1 MB array owned by the caller, I/O
//		goes through callbacks.
//
//		Covered: every 8086 opcode; 80186 PUSHA/POPA, BOUND, PUSH imm, IMUL
//		imm, INS/OUTS, ENTER/LEAVE, shifts by an immediate count; NEC
//		REPC/REPNC prefixes (64/65) and the 0F group (TEST1, CLR1, SET1,
//		NOT1, ADD4S, SUB4S, CMP4S, ROL4, ROR4, INS, EXT). Not covered: the
//		8080 emulation mode (BRKEM).
//
//		Registers use the 8086 encoding order:
//
//		  r[0] AX  r[1] CX  r[2] DX  r[3] BX  r[4] SP  r[5] BP  r[6] SI  r[7] DI
//		  sreg[0] ES  sreg[1] CS  sreg[2] SS  sreg[3] DS
//

#ifndef V30_H
#define V30_H

#include <stdint.h>

#define V30_CF 0x0001
#define V30_PF 0x0004
#define V30_AF 0x0010
#define V30_ZF 0x0040
#define V30_SF 0x0080
#define V30_TF 0x0100
#define V30_IF 0x0200
#define V30_DF 0x0400
#define V30_OF 0x0800

enum { V30_ES = 0, V30_CS, V30_SS, V30_DS };
enum { V30_AX = 0, V30_CX, V30_DX, V30_BX, V30_SP, V30_BP, V30_SI, V30_DI };

typedef struct
{
	uint16_t  r[8];
	uint16_t  sreg[4];
	uint16_t  ip;
	uint16_t  flags;
	int       halted;
	int       nmi;              // NMI line (edge)
	uint8_t  *mem;              // 1 MB, addresses wrap at 20 bits
	uint64_t  cycles;
	uint16_t  prev_ip, prev_cs; // the instruction being executed (logs)

	// I/O and interrupt callbacks
	uint8_t  ( *in8 )( uint16_t port );
	uint16_t ( *in16 )( uint16_t port );
	void     ( *out8 )( uint16_t port, uint8_t v );
	void     ( *out16 )( uint16_t port, uint16_t v );
	int      ( *irq_pending )( void );   // INT line
	int      ( *irq_ack )( void );       // returns the vector

	// decoder state (internal)
	int       seg_ovr;          // -1 or the overriding segment
	int       rep;              // 0, 0xF2, 0xF3, 0x64, 0x65
	int       inhibit;          // interrupts held off after MOV SS / POP SS / STI
	uint32_t  ea;               // linear address of the current memory operand
	uint16_t  ea_off;
	int       ea_seg;
	int       ea_reg;           // -1 = memory operand, else the register number
} v30_cpu;

/**
  * @brief  Reset: CS:IP = FFFF:0000, flags clear, interrupts off.
  *
  * @param  c The CPU (mem and callbacks are kept).
  *
  * @retval None.
  */
void v30_reset( v30_cpu *c );

/**
  * @brief  Execute instructions for about the given number of clock cycles.
  *
  * @param  c The CPU.
  * @param  cycles Clock cycles to run.
  *
  * @retval Cycles actually run.
  */
int v30_run( v30_cpu *c, int cycles );

/**
  * @brief  Disassemble-free one-line state dump for logs.
  *
  * @param  c The CPU.
  * @param  out Receives the text.
  * @param  n Room in out.
  *
  * @retval None.
  */
void v30_state( const v30_cpu *c, char *out, int n );

#endif
