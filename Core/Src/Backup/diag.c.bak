#include "diag.h"
#include "main.h"

static void put_char(char c)
{
    uint32_t guard = 2000000U;
    while (((USART3->ISR & USART_ISR_TXE_TXFNF) == 0U) && --guard) { }
    USART3->TDR = (uint8_t)c;
}

void Diag_Puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            put_char('\r');
        }
        put_char(*s++);
    }
}

void Diag_PutHex(uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    put_char('0');
    put_char('x');
    for (int i = 28; i >= 0; i -= 4) {
        put_char(digits[(v >> i) & 0xFU]);
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
        put_char(tmp[--n]);
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

    for (;;) { }
}
