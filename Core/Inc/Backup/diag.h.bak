#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>

/**
 * @file    diag.h
 * @brief   Низкоуровневая диагностика через UART3.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Опрашивающий (polling) вывод на уровне регистров UART3.
 * Безопасно вызывать из любого контекста (прерывание, обработчик сбоев, Error_Handler):
 * не использует stdio, malloc и состояние HAL.
 */
void Diag_Puts(const char *s);
void Diag_PutHex(uint32_t v);   /* Вывод 32-битного значения в формате "0x%08X" */
void Diag_PutDec(uint32_t v);   /* Вывод десятичного беззнакового числа */

/* Включение счётчика тактов DWT (DWT->CYCCNT), используемого для хронометража аудио-прерываний. */
void Diag_EnableCycleCounter(void);

/*
 * Вызывается (через naked-трамплин в stm32h7xx_it.c) из HardFault / MemManage / BusFault / UsageFault.
 * Указатель frame содержит сохранённые в стеке регистры: R0, R1, R2, R3, R12, LR, PC, xPSR.
 * Выводит содержимое регистров в UART3 и останавливает процессор (бесконечный цикл).
 */
void Diag_FaultReport(uint32_t *frame, uint32_t exc_return, uint32_t kind) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* DIAG_H */
