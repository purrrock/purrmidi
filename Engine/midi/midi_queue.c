#include "midi_queue.h"
#include <assert.h>

/**
 * Размер очереди должен быть строгой степенью двойки для быстрого
 * взятия остатка по модулю с помощью битовой маски (MIDI_QUEUE_SIZE - 1U).
 */
#define MIDI_QUEUE_SIZE 256U

_Static_assert((MIDI_QUEUE_SIZE != 0U) && ((MIDI_QUEUE_SIZE & (MIDI_QUEUE_SIZE - 1U)) == 0U),
               "MIDI_QUEUE_SIZE must be a power of two");

static MIDI_Event_t queue_buffer[MIDI_QUEUE_SIZE];
static volatile uint32_t head = 0;
static volatile uint32_t tail = 0;
static volatile uint32_t overrun_count = 0;

/*
 * SPSC-очередь: данные в queue_buffer[] пишутся ОБЫЧНЫМИ (не volatile) обращениями, а индексы
 * head/tail — volatile. volatile не является барьером для обычной памяти: компилятор вправе
 * переставить запись элемента после публикации head (и наоборот при чтении). Поэтому публикация
 * индекса идёт с release-семантикой, а чтение чужого индекса — с acquire.
 * GCC/Clang: __atomic_*; MSVC (Windows-сборка, x86 — строгий порядок): барьер компилятора.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define Q_LOAD_ACQUIRE(p)       __atomic_load_n((p), __ATOMIC_ACQUIRE)
#  define Q_STORE_RELEASE(p, v)   __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#  include <intrin.h>
#  define Q_LOAD_ACQUIRE(p)       (_ReadWriteBarrier(), *(p))
#  define Q_STORE_RELEASE(p, v)   (_ReadWriteBarrier(), *(p) = (v))
#endif

void MIDI_Queue_Init(void)
{
    head = 0;
    tail = 0;
    overrun_count = 0;
}

bool MIDI_Queue_Push(const MIDI_Event_t *event)
{
    uint32_t current_head = head;                       /* head пишет только производитель */
    uint32_t next_head = (current_head + 1U) & (MIDI_QUEUE_SIZE - 1U);

    if (next_head == Q_LOAD_ACQUIRE(&tail))
    {
        overrun_count++;
        return false;
    }

    queue_buffer[current_head] = *event;
    Q_STORE_RELEASE(&head, next_head);                  /* элемент виден потребителю только после этого */
    return true;
}

bool MIDI_Queue_Pop(MIDI_Event_t *event)
{
    uint32_t current_tail = tail;                       /* tail пишет только потребитель */

    if (current_tail == Q_LOAD_ACQUIRE(&head))
    {
        return false;
    }

    *event = queue_buffer[current_tail];
    Q_STORE_RELEASE(&tail, (current_tail + 1U) & (MIDI_QUEUE_SIZE - 1U));   /* слот освобождён после копирования */
    return true;
}

uint32_t MIDI_Queue_GetOverrunCount(void)
{
    return overrun_count;
}
