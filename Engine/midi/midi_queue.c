#include "midi_queue.h"
#include <assert.h>

#define MIDI_QUEUE_SIZE 256U

_Static_assert((MIDI_QUEUE_SIZE != 0U) && ((MIDI_QUEUE_SIZE & (MIDI_QUEUE_SIZE - 1U)) == 0U),
               "MIDI_QUEUE_SIZE must be a power of two");

static MIDI_Event_t queue_buffer[MIDI_QUEUE_SIZE];
static volatile uint32_t head = 0;
static volatile uint32_t tail = 0;
static volatile uint32_t overrun_count = 0;

void MIDI_Queue_Init(void)
{
    head = 0;
    tail = 0;
    overrun_count = 0;
}

bool MIDI_Queue_Push(const MIDI_Event_t *event)
{
    uint32_t current_head = head;
    uint32_t next_head = (current_head + 1U) & (MIDI_QUEUE_SIZE - 1U);

    if (next_head == tail)
    {
        overrun_count++;
        return false;
    }

    queue_buffer[current_head] = *event;
    head = next_head;
    return true;
}

bool MIDI_Queue_Pop(MIDI_Event_t *event)
{
    uint32_t current_tail = tail;

    if (current_tail == head)
    {
        return false;
    }

    *event = queue_buffer[current_tail];
    tail = (current_tail + 1U) & (MIDI_QUEUE_SIZE - 1U);
    return true;
}

uint32_t MIDI_Queue_GetOverrunCount(void)
{
    return overrun_count;
}
