#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Polling, register-level UART3 output. Safe to call from any context (ISR, fault handler,
 * Error_Handler): it does not use stdio, malloc or HAL state. */
void Diag_Puts(const char *s);
void Diag_PutHex(uint32_t v);   /* "0x%08X" */
void Diag_PutDec(uint32_t v);

/* Enables the DWT cycle counter (DWT->CYCCNT) used to time the audio interrupt. */
void Diag_EnableCycleCounter(void);

/* Called (via a naked trampoline in stm32h7xx_it.c) from HardFault / MemManage / BusFault /
 * UsageFault. frame = stacked R0,R1,R2,R3,R12,LR,PC,xPSR. Prints the registers and halts. */
void Diag_FaultReport(uint32_t *frame, uint32_t exc_return, uint32_t kind) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* DIAG_H */
