#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>

/**
 * @file    diag.h
 * @brief   Низкоуровневая диагностика через UART3 без динамической памяти и HAL.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Регистровый вывод строковых констант и чисел через USART3 (безопасно в ISR) */
void Diag_Puts(const char *s);
void Diag_PutHex(uint32_t v);   /* Вывод в формате "0x%08X" */
void Diag_PutDec(uint32_t v);   /* Вывод десятичного числа */

/* Включение счетчика тактов DWT (DWT->CYCCNT) для замера времени выполнения ISR */
void Diag_EnableCycleCounter(void);

/* Отчёт об аварийном останове (HardFault/MemManage/BusFault/UsageFault) */
void Diag_FaultReport(uint32_t *frame, uint32_t exc_return, uint32_t kind) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* DIAG_H */
