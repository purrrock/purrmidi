/*
 * Boot-time profiler for the organ voice (firmware only, not part of the host build).
 * Times single calls of the voice code on the real hardware, so that it is visible whether the time
 * goes into the tonewheel loop itself, into virtual dispatch, or into something else.
 */
#include <cstdio>
#include <cstdint>
#include "stm32h7xx.h"
#include "voice_organ.hpp"

using namespace synth;

extern "C" void OrganProfile_Run(void);

namespace {

constexpr uint32_t N = 2000;

volatile int32_t g_sink;
cc::value_t       g_drawbars[9] = {8, 8, 8, 8, 8, 8, 8, 8, 8};
uint32_t          g_counter = 123456;

void report(const char *name, uint32_t cycles_total, uint32_t overhead_total)
{
    const uint32_t net = (cycles_total > overhead_total) ? (cycles_total - overhead_total) : 0U;
    const uint32_t x10 = (uint32_t)(((uint64_t)net * 10U) / N);
    std::printf("[PROF] %-34s %lu.%lu cycles per call\r\n", name,
                (unsigned long)(x10 / 10U), (unsigned long)(x10 % 10U));
}

} // namespace

extern "C" void OrganProfile_Run(void)
{
    static voice::voice_organ v;
    voice::voice_base *b = &v;

    voice::voice_organ::init(g_drawbars, &g_counter);
    v.activate(60);
    for (int i = 0; i < 100; i++) { g_sink = v.get_sample(); }   /* warm-up: caches, attack phase */

    /* baseline: empty loop with the same bookkeeping */
    uint32_t t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < N; i++) { g_counter++; g_sink = (int32_t)i; }
    const uint32_t base = DWT->CYCCNT - t0;

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < N; i++) { g_counter++; g_sink = v.get_sample(); }
    report("voice_organ::get_sample() direct", DWT->CYCCNT - t0, base);

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < N; i++) { g_counter++; g_sink = b->get_sample(); }
    report("get_sample() via base pointer", DWT->CYCCNT - t0, base);

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < N; i++) { g_counter++; g_sink = v.is_active() ? 1 : 0; }
    report("voice_organ::is_active() direct", DWT->CYCCNT - t0, base);

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < N; i++) { g_counter++; g_sink = b->is_active() ? 1 : 0; }
    report("is_active() via base pointer", DWT->CYCCNT - t0, base);

    v.deactivate();
    v.reset();
    t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < N; i++) { g_counter++; g_sink = b->is_active() ? 1 : 0; }
    report("is_active() via base, voice idle", DWT->CYCCNT - t0, base);
}
