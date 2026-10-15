#include "usb_midi_packet.h"

#include <stddef.h>

bool UsbMidi_PacketToEvent(const uint8_t pkt[4], MIDI_Event_t *out)
{
    if (pkt == NULL || out == NULL) {
        return false;
    }

    const uint8_t cin    = pkt[0] & 0x0FU;
    const uint8_t status = pkt[1];

    /* Только голосовые сообщения канала. Статус обязан быть 0x80..0xEF (старший бит = 1, не 0xFx):
     * так отсекаются и «пустые» пакеты, и случайный мусор вместо статус-байта. */
    if (cin < 0x8U || cin > 0xEU) {
        return false;
    }
    if ((status & 0x80U) == 0U || status >= MIDI_STATUS_SYSTEM_EXCLUSIVE) {
        return false;
    }

    out->status = status;
    out->data1  = pkt[2] & 0x7FU;
    /* Program Change (0xC) и Channel Pressure (0xD) — двухбайтовые: третий байт не используется. */
    out->data2  = (cin == 0xCU || cin == 0xDU) ? 0U : (uint8_t)(pkt[3] & 0x7FU);
    return true;
}
