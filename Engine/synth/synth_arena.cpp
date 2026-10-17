#include "synth_arena.h"

#include <cstdint>
#include <cstring>

/* На STM32 арена лежит в отдельной секции .synth_arena (AXI SRAM, кэшируемая; см. STM32H743xx_FLASH.ld). Секция NOLOAD,
 * обнуляется startup-кодом ram_init.c, начальных значений у неё нет и быть не должно. */
#if defined(__GNUC__) && defined(__arm__)
#  define SYNTH_ARENA_SECTION __attribute__((section(".synth_arena")))
#else
#  define SYNTH_ARENA_SECTION
#endif

namespace {

alignas(32) std::uint8_t g_arena[SYNTH_ARENA_BYTES] SYNTH_ARENA_SECTION;
const void *volatile g_owner = nullptr;

}  // namespace

void *SynthArena_Acquire(const void *owner, std::size_t bytes)
{
    if (bytes > SYNTH_ARENA_BYTES) {
        return nullptr;
    }
    g_owner = nullptr;                    /* на время очистки арена ничья: IsOwner() == false */
    std::memset(g_arena, 0, bytes);
    g_owner = owner;
    return g_arena;
}

bool SynthArena_IsOwner(const void *owner)
{
    return owner != nullptr && g_owner == owner;
}

const void *SynthArena_Owner(void)
{
    return g_owner;
}
