// Тесты планировщика половин DMA-буфера (Engine/audio/audio_halves.c): порядок обработки, опоздания, тишина вместо
// повтора старого звука. Это логика, которая в прошивке разделяет прерывание DMA и рендер в PendSV.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "audio_halves.h"
#include "check.h"

// Нормальная работа: рендер успевает между запросами.
static void test_normal_alternation() {
    AudioHalves h;
    AudioHalves_Init(&h);
    unsigned half = 9;
    bool stale = true;

    CHECK(!AudioHalves_Next(&h, &half, &stale));        // делать нечего

    for (int i = 0; i < 6; ++i) {
        const unsigned want = (unsigned)(i & 1);
        CHECK(!AudioHalves_Request(&h, want));          // не опоздали
        CHECK(AudioHalves_Next(&h, &half, &stale));
        CHECK(half == want && !stale);
        AudioHalves_Done(&h, half);
        CHECK(!AudioHalves_Next(&h, &half, &stale));
    }
    CHECK(AudioHalves_LateCount(&h) == 0);
}

// Рендер половины 0 не закончился к моменту запроса половины 1: срок пропущен, DMA играет недозаполненную половину 0.
static void test_deadline_missed_while_rendering() {
    AudioHalves h;
    AudioHalves_Init(&h);
    unsigned half;
    bool stale;

    CHECK(!AudioHalves_Request(&h, 0));
    CHECK(AudioHalves_Next(&h, &half, &stale));         // PendSV начал рендер половины 0 ...
    CHECK(half == 0 && !stale);

    CHECK(AudioHalves_Request(&h, 1));                  // ... а DMA уже просит половину 1: опоздание
    CHECK(AudioHalves_LateCount(&h) == 1);

    AudioHalves_Done(&h, 0);                            // рендер 0 закончился (поздно, но закончился)
    CHECK(AudioHalves_Next(&h, &half, &stale));         // осталась только половина 1: рендерим по-настоящему
    CHECK(half == 1 && !stale);
    AudioHalves_Done(&h, 1);
    CHECK(!AudioHalves_Next(&h, &half, &stale));
}

// PendSV так и не стартовал (его задержали другие прерывания): ждут обе половины -> старшая заливается тишиной.
static void test_both_pending_older_is_silenced() {
    AudioHalves h;
    AudioHalves_Init(&h);
    unsigned half;
    bool stale;

    CHECK(!AudioHalves_Request(&h, 0));
    CHECK(AudioHalves_Request(&h, 1));                  // опоздание: половина 0 не тронута

    CHECK(AudioHalves_Next(&h, &half, &stale));
    CHECK(half == 0 && stale);                          // старшая (0) - тишина, не рендер
    AudioHalves_Done(&h, 0);

    CHECK(AudioHalves_Next(&h, &half, &stale));
    CHECK(half == 1 && !stale);                         // свежая (1) - настоящий звук
    AudioHalves_Done(&h, 1);
    CHECK(!AudioHalves_Next(&h, &half, &stale));
    CHECK(AudioHalves_LateCount(&h) == 1);

    // То же, но старшая - половина 1.
    CHECK(!AudioHalves_Request(&h, 1));
    CHECK(AudioHalves_Request(&h, 0));
    CHECK(AudioHalves_Next(&h, &half, &stale));
    CHECK(half == 1 && stale);
    AudioHalves_Done(&h, 1);
    CHECK(AudioHalves_Next(&h, &half, &stale));
    CHECK(half == 0 && !stale);
}

// Одна и та же половина запрошена дважды без обработки (рендер отстал больше чем на период).
static void test_same_half_requested_twice() {
    AudioHalves h;
    AudioHalves_Init(&h);
    unsigned half;
    bool stale;

    CHECK(!AudioHalves_Request(&h, 0));
    CHECK(AudioHalves_Request(&h, 0));                  // повторный запрос той же половины - опоздание
    CHECK(AudioHalves_LateCount(&h) == 1);
    CHECK(AudioHalves_Next(&h, &half, &stale));
    CHECK(half == 1 - 1 && !stale);                     // pending == только бит 0
    AudioHalves_Done(&h, 0);
    CHECK(!AudioHalves_Next(&h, &half, &stale));
}

// «Прерывание» (запросы по таймеру) и «PendSV» (обработка) в разных потоках: ничего не зависает, все запросы
// либо обработаны, либо учтены как опоздание, а после остановки очередь пуста. Осмысленно под ThreadSanitizer.
static void test_threads() {
    AudioHalves h;
    AudioHalves_Init(&h);
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> processed{0}, silenced{0};

    std::thread pendsv([&]() {
        unsigned half;
        bool stale;
        while (!stop.load(std::memory_order_acquire)) {
            while (AudioHalves_Next(&h, &half, &stale)) {
                if (stale) silenced.fetch_add(1); else processed.fetch_add(1);
                AudioHalves_Done(&h, half);
            }
            std::this_thread::yield();
        }
        while (AudioHalves_Next(&h, &half, &stale)) {   // дочитать остаток
            if (stale) silenced.fetch_add(1); else processed.fetch_add(1);
            AudioHalves_Done(&h, half);
        }
    });

    const uint32_t kRequests = 20000;
    uint32_t late_seen = 0;
    for (uint32_t i = 0; i < kRequests; ++i) {
        if (AudioHalves_Request(&h, i & 1U)) ++late_seen;
        if ((i % 7) == 0) std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    pendsv.join();

    unsigned half;
    bool stale;
    CHECK(!AudioHalves_Next(&h, &half, &stale));                    // очередь пуста
    CHECK(AudioHalves_LateCount(&h) == late_seen);                  // счётчик совпадает с возвратами Request()
    CHECK(processed.load() + silenced.load() >= 1);                 // что-то обработано
    CHECK(processed.load() + silenced.load() <= kRequests);         // никогда не больше, чем запросов
}

int main() {
    test_normal_alternation();
    test_deadline_missed_while_rendering();
    test_both_pending_older_is_silenced();
    test_same_half_requested_twice();
    test_threads();

    if (purrmidi_test::Failures() != 0) {
        std::fprintf(stderr, "test_audio_halves: %d check(s) failed\n", purrmidi_test::Failures());
        return 1;
    }
    std::puts("test_audio_halves: OK");
    return 0;
}
