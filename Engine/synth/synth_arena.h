/**
 * @file    synth_arena.h
 * @brief   Общая арена памяти для голосов звуковых движков.
 *
 * Одновременно звучит только один движок, поэтому голоса всех движков (Pluck 67 КБ, Violin ≈42 КБ,
 * Lead 2 КБ) не нужно держать в памяти одновременно: они делят одну область размером с самый большой.
 * Движок запрашивает арену в своём Init() (SynthArena_NewArray), и всё, что лежало там до этого, пропадает —
 * так и задумано: переключение движка (synth_engine.cpp: activate) выполняется под замком, а предыдущий движок после
 * него уже не вызывается.
 *
 * Бонусы:
 *  - Конструкторы голосов выполняются явно (placement new) при каждом Init(), а не один раз при старте. Статические
 *    объекты в NOLOAD-секциях не получают начальных значений (startup их не копирует), арена от этого не зависит.
 *  - Память арены не пересекается с DMA-буферами (.dma_buffer) и кэшируема (AXI SRAM, см. xip_mpu.c): линии
 *    задержки физических моделей читаются/пишутся последовательно и хорошо ложатся в D-cache.
 *
 * Потокобезопасность: SynthArena_Acquire() вызывается только из Init() движка (main, под замком смены движка).
 * SynthArena_IsOwner() можно вызывать из аудио-контекста: это одно чтение указателя.
 */
#ifndef SYNTH_ARENA_H
#define SYNTH_ARENA_H

#include <cstddef>
#include <new>

/** Размер арены. Должен быть не меньше самого большого набора голосов; проверяется static_assert'ом в SynthArena_NewArray. */
constexpr std::size_t SYNTH_ARENA_BYTES = 144U * 1024U;

/**
 * Отдать арену движку `owner` (адрес любого его статического объекта - уникальный идентификатор владельца).
 * Память выровнена на 32 байта и обнулена (запрошенные `bytes` байт). Данные прежнего владельца теряются.
 * @return указатель на арену или nullptr, если bytes > SYNTH_ARENA_BYTES.
 */
void *SynthArena_Acquire(const void *owner, std::size_t bytes);

/** true - арена сейчас принадлежит `owner` (его данные не затёрты другим движком). */
bool SynthArena_IsOwner(const void *owner);

/** Текущий владелец (nullptr, если арена ещё не выдавалась). Для диагностики и тестов. */
const void *SynthArena_Owner(void);

/**
 * Создаёт в арене массив из N объектов T (вызываются конструкторы по умолчанию: значения по умолчанию членов,
 * линии задержки и т.п.). Возвращает nullptr, если не поместилось - но для этого есть static_assert: добавили
 * движок побольше - компилятор подскажет увеличить SYNTH_ARENA_BYTES.
 */
template <typename T, std::size_t N>
T *SynthArena_NewArray(const void *owner)
{
    static_assert(sizeof(T) * N <= SYNTH_ARENA_BYTES,
                  "Voices of this engine do not fit into the shared arena: increase SYNTH_ARENA_BYTES (synth_arena.h)");
    void *mem = SynthArena_Acquire(owner, sizeof(T) * N);
    if (mem == nullptr) {
        return nullptr;
    }
    T *arr = static_cast<T *>(mem);
    for (std::size_t i = 0; i < N; ++i) {
        ::new (static_cast<void *>(&arr[i])) T();
    }
    return arr;
}

#endif /* SYNTH_ARENA_H */
