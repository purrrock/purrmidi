#ifndef USB_MIDI_PACKET_H
#define USB_MIDI_PACKET_H

#include <stdbool.h>
#include <stdint.h>
#include "midi_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file    usb_midi_packet.h
 * @brief   Разбор одного 4-байтового USB-MIDI Event Packet (USB MIDI 1.0, §4) в MIDI_Event_t.
 *
 * Чистая функция без обращений к железу: её можно тестировать на хосте и использовать из
 * любого контекста (USB-callback, main, тесты).
 *
 * Пропускаются ТОЛЬКО сообщения голосового канала (Code Index Number 0x8..0xE: Note Off/On,
 * Poly Pressure, Control Change, Program Change, Channel Pressure, Pitch Bend).
 * Остальное отбрасывается: SysEx (CIN 0x4..0x7), System Common (0x2/0x3), одиночные байты и
 * System Real-Time (CIN 0xF: Clock 0xF8 = 24 раза за такт, Active Sensing 0xFE каждые ~300 мс)
 * — они не нужны синтезатору, но забивали бы очередь событий и могли вытеснить Note Off.
 *
 * Байты данных маскируются до 7 бит (0x7F): неисправное устройство не сможет передать
 * движкам «ноту 200» и вывести индекс за пределы таблиц.
 *
 * @param pkt  4 байта пакета: [cable<<4 | CIN, status, data1, data2].
 * @param out  Результат (меняется только при возврате true).
 * @return true — пакет содержит событие голосового канала и записан в *out.
 */
bool UsbMidi_PacketToEvent(const uint8_t pkt[4], MIDI_Event_t *out);

#ifdef __cplusplus
}
#endif

#endif /* USB_MIDI_PACKET_H */
