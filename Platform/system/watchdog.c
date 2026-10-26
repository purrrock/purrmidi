#include "watchdog.h"

#include <stdio.h>

#include "main.h"

/* Тактирование IWDG - LSI (номинально 32 кГц, разброс по даташиту около ±10%; временные значения ниже приблизительные).
 * Предделитель /64 -> 500 Гц, 12-битный счётчик: максимум 4095 / 500 = 8,19 с. */
#define IWDG_KEY_RELOAD         0xAAAAU
#define IWDG_KEY_ENABLE         0xCCCCU
#define IWDG_KEY_WRITE_ACCESS   0x5555U
#define IWDG_PRESCALER_DIV64    0x4U

#define WDG_RELOAD_BOOT         4095U       /* ~8,2 с */
#define WDG_RELOAD_RUNTIME      750U        /* ~1,5 с */

#define WDG_REG_UPDATE_TIMEOUT_MS   100U

static uint32_t s_reset_flags;              /* RCC->RSR на момент старта */

/* Ждёт, пока значения PR/RLR/WINR дойдут до домена LSI (флаги SR). Ждать приходится ~5 тактов LSI (~160 мкс). */
static void wait_registers_updated(void)
{
    const uint32_t t0 = HAL_GetTick();
    while (IWDG1->SR != 0U)
    {
        if ((HAL_GetTick() - t0) > WDG_REG_UPDATE_TIMEOUT_MS)
        {
            break;      /* таймер всё равно работает (с прежними значениями): лучше так, чем зависнуть здесь */
        }
    }
}

static void set_reload(uint32_t reload)
{
    IWDG1->KR  = IWDG_KEY_WRITE_ACCESS;
    IWDG1->RLR = reload;
    wait_registers_updated();
    IWDG1->KR  = IWDG_KEY_RELOAD;
}

void Watchdog_Start(void)
{
    /* Причину сброса читаем ДО запуска таймера и очищаем флаги, чтобы следующий сброс определился однозначно. */
    s_reset_flags = RCC->RSR;
    RCC->RSR |= RCC_RSR_RMVF;

    __HAL_DBGMCU_FREEZE_IWDG1();            /* остановка ядра отладчиком останавливает и таймер */

    IWDG1->KR  = IWDG_KEY_ENABLE;           /* пуск (LSI включается аппаратно) */
    IWDG1->KR  = IWDG_KEY_WRITE_ACCESS;
    IWDG1->PR  = IWDG_PRESCALER_DIV64;
    IWDG1->RLR = WDG_RELOAD_BOOT;
    IWDG1->WINR = 0x0FFFU;                  /* окно отключено: «кормить» можно в любой момент */
    wait_registers_updated();
    IWDG1->KR  = IWDG_KEY_RELOAD;
}

void Watchdog_EnterRuntime(void)
{
    set_reload(WDG_RELOAD_RUNTIME);
}

void Watchdog_Kick(void)
{
    IWDG1->KR = IWDG_KEY_RELOAD;
}

uint32_t Watchdog_GetResetFlags(void)
{
    return s_reset_flags;
}

void Watchdog_ReportResetCause(void)
{
    const uint32_t f = s_reset_flags;
    const char *cause;

    /* Несколько флагов бывают взведены одновременно (при включении питания: POR + PIN + BOR) - берём главный. */
    if      (f & RCC_RSR_IWDG1RSTF) cause = "WATCHDOG (IWDG1): main loop or audio stopped";
    else if (f & RCC_RSR_WWDG1RSTF) cause = "window watchdog (WWDG1)";
    else if (f & RCC_RSR_SFTRSTF)   cause = "software reset (NVIC_SystemReset: fault or NMI recovery)";
    else if (f & RCC_RSR_LPWRRSTF)  cause = "illegal low-power mode entry";
    else if (f & RCC_RSR_PORRSTF)   cause = "power-on";
    else if (f & RCC_RSR_BORRSTF)   cause = "brown-out";
    else if (f & RCC_RSR_PINRSTF)   cause = "reset pin (NRST)";
    else                            cause = "unknown";

    printf("[RESET] cause: %s (RSR=0x%08lX)\r\n", cause, (unsigned long)f);
}
