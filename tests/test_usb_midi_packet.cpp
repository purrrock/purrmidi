// Тесты разбора USB-MIDI Event Packet (Engine/midi/usb_midi_packet.c) — той самой логики,
// которая в прошивке работает в USBH_MIDI_ReceiveCallback().
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "usb_midi_packet.h"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static bool parse(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, MIDI_Event_t *e)
{
    const uint8_t pkt[4] = {b0, b1, b2, b3};
    return UsbMidi_PacketToEvent(pkt, e);
}

static void test_channel_voice()
{
    MIDI_Event_t e{};
    CHECK(parse(0x09, 0x90, 60, 100, &e));
    CHECK(e.status == 0x90 && e.data1 == 60 && e.data2 == 100);

    CHECK(parse(0x08, 0x83, 61, 7, &e));
    CHECK(e.status == 0x83 && e.data1 == 61 && e.data2 == 7);

    CHECK(parse(0x0B, 0xB0, 64, 127, &e));
    CHECK(e.status == 0xB0 && e.data1 == 64 && e.data2 == 127);

    CHECK(parse(0x0E, 0xE0, 0x00, 0x40, &e));          // pitch bend
    CHECK(e.status == 0xE0 && e.data1 == 0 && e.data2 == 0x40);

    CHECK(parse(0x1B, 0xB0, 7, 99, &e));               // cable 1 допустим, номер кабеля игнорируется
    CHECK(e.data1 == 7 && e.data2 == 99);
}

static void test_two_byte_messages()
{
    MIDI_Event_t e{};
    CHECK(parse(0x0C, 0xC0, 5, 0x55, &e));             // Program Change: третий байт не используется
    CHECK(e.status == 0xC0 && e.data1 == 5 && e.data2 == 0);

    CHECK(parse(0x0D, 0xD2, 90, 0x7F, &e));            // Channel Pressure
    CHECK(e.status == 0xD2 && e.data1 == 90 && e.data2 == 0);
}

static void test_data_bytes_are_masked_to_7_bits()
{
    MIDI_Event_t e{};
    CHECK(parse(0x09, 0x90, 200, 0xFF, &e));           // неисправное устройство: «нота 200»
    CHECK(e.data1 == (200 & 0x7F) && e.data2 == 0x7F);
    CHECK(e.data1 < 128 && e.data2 < 128);
}

static void test_non_channel_messages_are_dropped()
{
    MIDI_Event_t e{0xAA, 0xBB, 0xCC};
    CHECK(!parse(0x00, 0x90, 60, 100, &e));            // CIN 0 — пустой пакет
    CHECK(!parse(0x0F, 0xF8, 0, 0, &e));               // MIDI Clock
    CHECK(!parse(0x0F, 0xFE, 0, 0, &e));               // Active Sensing
    CHECK(!parse(0x04, 0xF0, 0x7E, 0x7F, &e));         // SysEx start
    CHECK(!parse(0x07, 0x01, 0xF7, 0, &e));            // SysEx end (3 байта)
    CHECK(!parse(0x05, 0xF7, 0, 0, &e));               // SysEx end (1 байт)
    CHECK(!parse(0x02, 0xF1, 0x10, 0, &e));            // System Common (MTC)
    CHECK(!parse(0x03, 0xF2, 0x10, 0x20, &e));         // Song Position
    CHECK(e.status == 0xAA && e.data1 == 0xBB && e.data2 == 0xCC);   // *out не тронут при отказе
}

static void test_garbage_status_is_dropped()
{
    MIDI_Event_t e{};
    CHECK(!parse(0x09, 0x3C, 60, 100, &e));            // «статус» без старшего бита
    CHECK(!parse(0x09, 0xF0, 60, 100, &e));            // 0xFx в канальном пакете
    CHECK(!UsbMidi_PacketToEvent(nullptr, &e));
    const uint8_t pkt[4] = {0x09, 0x90, 60, 100};
    CHECK(!UsbMidi_PacketToEvent(pkt, nullptr));
}

int main()
{
    test_channel_voice();
    test_two_byte_messages();
    test_data_bytes_are_masked_to_7_bits();
    test_non_channel_messages_are_dropped();
    test_garbage_status_is_dropped();

    if (g_failures != 0) {
        std::fprintf(stderr, "test_usb_midi_packet: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("test_usb_midi_packet: OK");
    return 0;
}
