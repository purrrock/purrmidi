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
 * Вывод в UART3 на уровне регистров, без stdio и malloc. Безопасно вызывать из прерывания (PendSV аудио-рендера),
 * из обработчиков сбоев и из Error_Handler.
 *
 * Согласование с printf основного цикла: если основной цикл прямо сейчас передаёт данные или не закончил строку,
 * текст из прерывания не пишется в UART, а откладывается в кольцевой буфер (256 байт, при переполнении лишнее
 * теряется) и выводится основным циклом на границе строки. Иначе вывод оставался бы перемешанным. Если UART свободен,
 * текст уходит сразу. После Diag_EnterPanic() вывод всегда прямой.
 */
void Diag_Puts(const char *s);
void Diag_PutHex(uint32_t v);   /* Вывод 32-битного значения в формате "0x%08X" */
void Diag_PutDec(uint32_t v);   /* Вывод десятичного беззнакового числа */

/* Аварийный режим (HardFault, Error_Handler): дальше только прямой вывод, без буфера и без ожидания основного цикла. */
void Diag_EnterPanic(void);

/*
 * Только основной контекст. Запись printf-данных в UART3 (используется _write): помечает UART занятым, передаёт данные,
 * а в конце строки выводит накопленное прерываниями. Возвращает len или 0 при ошибке/таймауте.
 */
int Diag_Write(const char *ptr, int len);

/* Только основной контекст, вызывать в главном цикле: выводит отложенные сообщения из прерываний. */
void Diag_Flush(void);

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
