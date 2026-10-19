/**
 * @file    ram_init.c
 * @brief   Обнуление AXI SRAM и D2 SRAM целиком до запуска C++-конструкторов.
 *
 * Секции .dma_buffer (D2), .synth_arena и .axi_heap (AXI) помечены в линкер-скрипте как NOLOAD: startup-код их не
 * обнуляет, после включения питания там случайные данные. Обнуляются обе области целиком (а не только занятые
 * секции): так любая будущая секция в них тоже получает корректные ECC-биты. На STM32H7 AXI SRAM защищена ECC (слово 64 бита, у остальных SRAM - 32+7):
 * проверочные биты в неинициализированной памяти неверны, поэтому первая же неполная запись (байт, полуслово или
 * 32 бита в 64-битное слово AXI) требует чтения-модификации-записи старого слова и может дать ECC-ошибку
 * (NMI/BusFault). ST рекомендует перед использованием записать всю память целыми 64-битными словами - это и делает
 * ram_init(). Заодно данные получают предсказуемое начальное значение 0 (как у обычной .bss).
 *
 * Запуск: через .preinit_array, то есть из __libc_init_array() ПЕРЕД всеми статическими конструкторами C++ (объекты
 * в этих секциях конструируются позже и не затираются). Startup-файл CubeMX менять не нужно.
 * Стоимость: 0,8 МБ записей, порядка 1-3 мс.
 */
#include <stdint.h>

/* Границы областей задаёт линкер-скрипт (STM32H743xx_FLASH.ld): ORIGIN/LENGTH регионов RAM и RAM_D2. */
extern uint8_t _sram_axi[], _eram_axi[];
extern uint8_t _sram_d2[], _eram_d2[];

/* Запись нулей 64-битными словами (STRD). Вызов memset() не годится: он пишет по 8/32 бит. */
static void zero_ecc_safe(uint8_t *start, uint8_t *end)
{
    uintptr_t p = (uintptr_t)start;
    const uintptr_t e = (uintptr_t)end;

    /* На случай невыровненных границ: хвост/голову дописываем 32-битными записями (границы секций выровнены,
     * так что на практике эти циклы не выполняются). */
    while ((p & 7U) != 0U && p + 4U <= e)
    {
        *(volatile uint32_t *)p = 0U;
        p += 4U;
    }
    if (p + 8U <= e)
    {
        uintptr_t q = p + ((e - p) & ~(uintptr_t)7U);       /* конец области, кратный 8 */
        register uint32_t zero_lo __asm__("r2") = 0U;
        register uint32_t zero_hi __asm__("r3") = 0U;
        __asm__ volatile(
            "1:                             \n"
            "   strd   %[lo], %[hi], [%[p]], #8 \n"
            "   cmp    %[p], %[q]           \n"
            "   bcc    1b                   \n"
            : [p] "+r"(p)
            : [q] "r"(q), [lo] "r"(zero_lo), [hi] "r"(zero_hi)
            : "cc", "memory");
    }
    while (p + 4U <= e)
    {
        *(volatile uint32_t *)p = 0U;
        p += 4U;
    }
}

static void ram_init(void)
{
    zero_ecc_safe(_sram_axi, _eram_axi);
    zero_ecc_safe(_sram_d2, _eram_d2);
}

/* Запись в .preinit_array: __libc_init_array() вызывает её раньше .init_array (конструкторов C++). */
__attribute__((section(".preinit_array"), used))
static void (*const s_ram_init_entry)(void) = ram_init;
