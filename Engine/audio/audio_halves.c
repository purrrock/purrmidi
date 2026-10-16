#include "audio_halves.h"

/* GCC/Clang (arm-none-eabi, Linux, MinGW): __atomic_*. MSVC (Windows-сборка тестов, x86 - строгий порядок):
 * Interlocked-операции со встроенным полным барьером. */
#if defined(__GNUC__) || defined(__clang__)
#  define AH_LOAD_ACQ(p)        __atomic_load_n((p), __ATOMIC_ACQUIRE)
#  define AH_STORE_REL(p, v)    __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#  define AH_FETCH_OR(p, v)     __atomic_fetch_or((p), (v), __ATOMIC_ACQ_REL)
#  define AH_FETCH_AND(p, v)    __atomic_fetch_and((p), (v), __ATOMIC_ACQ_REL)
#  define AH_FETCH_ADD(p, v)    __atomic_fetch_add((p), (v), __ATOMIC_RELAXED)
#else
#  include <intrin.h>
#  define AH_LOAD_ACQ(p)        (_ReadWriteBarrier(), *(p))
#  define AH_STORE_REL(p, v)    (_ReadWriteBarrier(), *(p) = (v))
#  define AH_FETCH_OR(p, v)     ((uint32_t)_InterlockedOr((volatile long *)(p), (long)(v)))
#  define AH_FETCH_AND(p, v)    ((uint32_t)_InterlockedAnd((volatile long *)(p), (long)(v)))
#  define AH_FETCH_ADD(p, v)    ((uint32_t)_InterlockedExchangeAdd((volatile long *)(p), (long)(v)))
#endif

void AudioHalves_Init(AudioHalves *h)
{
    h->pending  = 0U;
    h->last_req = 0U;
    h->late     = 0U;
}

bool AudioHalves_Request(AudioHalves *h, unsigned half)
{
    half &= 1U;
    const uint32_t bit   = 1U << half;
    const uint32_t other = 1U << (half ^ 1U);

    /* last_req публикуем ДО флага: читатель, увидев флаг, увидит и актуальный last_req. */
    AH_STORE_REL(&h->last_req, half);
    const uint32_t before = AH_FETCH_OR(&h->pending, bit);

    /* Предыдущая половина ещё не готова (DMA уже читает её) или эта же так и не обработана -> опоздание. */
    const bool late = (before & (other | bit)) != 0U;
    if (late) {
        (void)AH_FETCH_ADD(&h->late, 1U);
    }
    return late;
}

bool AudioHalves_Next(AudioHalves *h, unsigned *half, bool *stale)
{
    const uint32_t pend = AH_LOAD_ACQ(&h->pending);
    if (pend == 0U) {
        return false;
    }
    if (pend == 3U) {
        /* Ждут обе: более старая - та, что запрошена не последней. Она уже играется - заливаем её тишиной. */
        *half  = (unsigned)(AH_LOAD_ACQ(&h->last_req) ^ 1U);
        *stale = true;
    } else {
        *half  = (pend & 1U) ? 0U : 1U;
        *stale = false;
    }
    return true;
}

void AudioHalves_Done(AudioHalves *h, unsigned half)
{
    (void)AH_FETCH_AND(&h->pending, ~(1U << (half & 1U)));
}

uint32_t AudioHalves_LateCount(const AudioHalves *h)
{
    return AH_LOAD_ACQ(&h->late);
}
