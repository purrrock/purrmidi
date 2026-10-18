#include "diag.h"
#include "main.h"
#include "usart.h"
#include <stdio.h>

/*
 * Вывод в UART3 из двух контекстов, которые раньше мешали друг другу:
 *   - основной цикл: printf -> _write -> Diag_Write -> HAL_UART_Transmit (может быть вытеснен PendSV в любой момент);
 *   - прерывание (PendSV аудио-рендера): Diag_Puts/PutDec/PutHex.
 * Если PendSV вытеснил основной цикл посреди передачи (или посреди строки printf), прямая запись в TDR перемешала бы
 * символы двух сообщений. Поэтому в такой момент текст из прерывания не пишется в UART, а кладётся в кольцевой буфер;
 * основной цикл сливает его сам, на границе строки (конец _write со '\n' или Diag_Flush()). Если основной цикл UART
 * сейчас не использует, прерывание пишет напрямую - так «последний вздох» уходит сразу, даже когда основной цикл
 * зависает. В аварийном режиме (HardFault, Error_Handler) всегда прямой вывод.
 */

#define DIAG_RING_SIZE   256U           /* степень двойки */
#define DIAG_FLUSH_CHUNK 64U
#define DIAG_STALE_MS    100U           /* строка без '\n' дольше этого срока не задерживает вывод из прерывания */

static volatile uint32_t g_ring_head;   /* пишет прерывание (единственный производитель) */
static volatile uint32_t g_ring_tail;   /* пишет основной цикл (единственный потребитель) */
static volatile uint32_t g_ring_dropped;
static char              g_ring[DIAG_RING_SIZE];

static volatile uint8_t  g_tx_busy;     /* основной цикл внутри HAL_UART_Transmit / слива буфера */
static volatile uint8_t  g_line_open;   /* основной цикл напечатал часть строки (последний байт не '\n') */
static volatile uint8_t  g_panic;       /* аварийный режим: только прямой вывод */
static volatile uint32_t g_last_write_tick;

static void put_char(char c)
{
    uint32_t guard = 2000000U;
    while (((USART3->ISR & USART_ISR_TXE_TXFNF) == 0U) && --guard) { }
    USART3->TDR = (uint8_t)c;
}

static void put_char_nl(char c)
{
    if (c == '\n') {
        put_char('\r');
    }
    put_char(c);
}

static int ring_empty(void)
{
    return g_ring_head == g_ring_tail;
}

static void ring_push(char c)
{
    const uint32_t head = g_ring_head;
    if (((head + 1U) & (DIAG_RING_SIZE - 1U)) == g_ring_tail) {
        g_ring_dropped++;               /* буфер полон: теряем символ, но не блокируем прерывание */
        return;
    }
    g_ring[head] = c;
    __DMB();
    g_ring_head = (head + 1U) & (DIAG_RING_SIZE - 1U);
}

/* Единая точка вывода символа для Diag_Puts / PutHex / PutDec. */
static void emit_char(char c)
{
    if (g_panic) {
        put_char_nl(c);
    } else if (g_tx_busy || g_line_open || !ring_empty()) {
        ring_push(c);                   /* ring не пуст: прямой вывод обогнал бы уже поставленные в очередь символы */
    } else {
        put_char_nl(c);
    }
}

void Diag_EnterPanic(void)
{
    g_panic = 1U;
}

/* Основной контекст. Выводит накопленное из прерываний; вызывается, когда UART не занят и строка закрыта. */
static void drain_ring(void)
{
    char chunk[DIAG_FLUSH_CHUNK * 2U];
    while (!ring_empty()) {
        uint32_t n = 0U;
        while (!ring_empty() && n < DIAG_FLUSH_CHUNK) {
            const char c = g_ring[g_ring_tail];
            __DMB();
            g_ring_tail = (g_ring_tail + 1U) & (DIAG_RING_SIZE - 1U);
            if (c == '\n') {
                chunk[n++] = '\r';
            }
            chunk[n++] = c;
        }
        (void)HAL_UART_Transmit(&huart3, (uint8_t *)chunk, (uint16_t)n, (n * 10U) / 115U + 20U);
    }
}

int Diag_Write(const char *ptr, int len)
{
    if (len <= 0) {
        return 0;
    }
    g_tx_busy = 1U;
    const uint32_t timeout = (uint32_t)(((uint32_t)len * 10U) / 115U) + 20U;
    const HAL_StatusTypeDef st = HAL_UART_Transmit(&huart3, (uint8_t *)ptr, (uint16_t)len, timeout);
    g_last_write_tick = HAL_GetTick();
    g_line_open = (ptr[len - 1] != '\n') ? 1U : 0U;
    if (!g_line_open) {
        drain_ring();                   /* строка закончена: можно выдать то, что накопило прерывание */
    }
    g_tx_busy = 0U;
    return (st == HAL_OK) ? len : 0;
}

void Diag_Flush(void)
{
    if (ring_empty() || g_tx_busy) {
        return;
    }
    if (g_line_open && (HAL_GetTick() - g_last_write_tick) < DIAG_STALE_MS) {
        return;                         /* ждём конца текущей строки printf */
    }
    g_tx_busy = 1U;
    drain_ring();
    g_tx_busy = 0U;

    const uint32_t dropped = g_ring_dropped;
    if (dropped != 0U) {
        g_ring_dropped = 0U;
        printf("[DIAG] %lu chars lost (ring full)\n", (unsigned long)dropped);
    }
}

void Diag_Puts(const char *s)
{
    while (*s) {
        emit_char(*s++);
    }
}

void Diag_PutHex(uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    emit_char('0');
    emit_char('x');
    for (int i = 28; i >= 0; i -= 4) {
        emit_char(digits[(v >> i) & 0xFU]);
    }
}

void Diag_PutDec(uint32_t v)
{
    char tmp[11];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (n > 0) {
        emit_char(tmp[--n]);
    }
}

void Diag_EnableCycleCounter(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55U;            /* unlock (Cortex-M7) */
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void field(const char *name, uint32_t v)
{
    Diag_Puts(" ");
    Diag_Puts(name);
    Diag_Puts("=");
    Diag_PutHex(v);
}

void Diag_FaultReport(uint32_t *frame, uint32_t exc_return, uint32_t kind)
{
    static const char *const names[] = { "HardFault", "MemManage", "BusFault", "UsageFault" };

    __disable_irq();
    Diag_EnterPanic();                 /* только прямой вывод: основной цикл мог умереть посреди передачи */
    Diag_Puts("\n*** ");
    Diag_Puts(names[kind & 3U]);
    Diag_Puts(" ***\n");

    field("PC", frame[6]);
    field("LR", frame[5]);
    field("xPSR", frame[7]);
    Diag_Puts("\n");
    field("R0", frame[0]);
    field("R1", frame[1]);
    field("R2", frame[2]);
    field("R3", frame[3]);
    field("R12", frame[4]);
    Diag_Puts("\n");
    field("CFSR", SCB->CFSR);
    field("HFSR", SCB->HFSR);
    field("MMFAR", SCB->MMFAR);
    field("BFAR", SCB->BFAR);
    Diag_Puts("\n");
    field("EXC_RETURN", exc_return);
    field("SP", (uint32_t)frame);
    field("ICSR", SCB->ICSR);          /* VECTACTIVE (bits 8:0) = interrupt that was running */
    Diag_Puts("\n");
    Diag_Puts("decode: arm-none-eabi-addr2line -f -C -e build/Release/purrmidi.elf <PC> <LR>\n");

#ifndef PURRMIDI_FAULT_HALT
    /* Даём отчёту уйти в UART (~1 с, IRQ выключены и SysTick не идёт, поэтому счётный цикл), затем
     * перезапускаем: музыкальный инструмент не должен молча зависать после сбоя. Для отладки по
     * SWD соберите с -DPURRMIDI_FAULT_HALT=1 - тогда останется бесконечный цикл. */
    for (volatile uint32_t i = 0; i < (SystemCoreClock / 4U); ++i) { __NOP(); }
    NVIC_SystemReset();
#endif
    for (;;) { }
}
