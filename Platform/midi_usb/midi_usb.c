#include "midi_usb.h"
#include "main.h"
#include "usb_host.h"
#include "usbh_midi.h"
#include "midi_event.h"
#include "midi_queue.h"
#include "display.h"

extern ApplicationTypeDef Appli_state;
extern USBH_HandleTypeDef hUsbHostFS;

__ALIGN_BEGIN static uint8_t midi_rx_buffer[64] __ALIGN_END;

static volatile uint32_t midi_usb_packets = 0;
static volatile uint32_t midi_events = 0;
static volatile uint32_t midi_receive_errors = 0;

static ApplicationTypeDef previous_state = APPLICATION_IDLE;

void MIDI_USB_Init(void)
{
    midi_usb_packets = 0;
    midi_events = 0;
    midi_receive_errors = 0;
    previous_state = APPLICATION_IDLE;
}

void USBH_MIDI_ReceiveCallback(USBH_HandleTypeDef *phost)
{
    midi_usb_packets++;
    uint16_t length = USBH_MIDI_GetLastReceivedDataSize(phost);

    for (uint16_t i = 0; i + 3 < length; i += 4)
    {
        uint8_t cin = midi_rx_buffer[i] & 0x0F;

        if (cin == 0x00)
        {
            continue;
        }

        uint8_t status = midi_rx_buffer[i + 1];
        uint8_t data1  = 0;
        uint8_t data2  = 0;

        switch (cin)
        {
            case 0x1:
            case 0x5:
            case 0xF:
                // 1 byte message (status only)
                break;

            case 0x2:
            case 0x6:
            case 0xC:
            case 0xD:
                // 2 byte message (status + data1)
                data1 = midi_rx_buffer[i + 2];
                break;

            case 0x3:
            case 0x4:
            case 0x7:
            case 0x8:
            case 0x9:
            case 0xA:
            case 0xB:
            case 0xE:
                // 3 byte message (status + data1 + data2)
                data1 = midi_rx_buffer[i + 2];
                data2 = midi_rx_buffer[i + 3];
                break;

            default:
                continue;
        }

        midi_events++;
        MIDI_Event_t event = {
            .status = status,
            .data1 = data1,
            .data2 = data2
        };
        MIDI_Queue_Push(&event);
    }
}

void MIDI_USB_Process(void)
{
    if (Appli_state != previous_state)
    {
        Display_SetMidiConnected(Appli_state == APPLICATION_READY);

        if (Appli_state == APPLICATION_READY)
        {
            if (USBH_MIDI_Receive(&hUsbHostFS,
                                  midi_rx_buffer,
                                  sizeof(midi_rx_buffer)) != USBH_OK)
            {
                midi_receive_errors++;
            }
        }
        previous_state = Appli_state;
    }
}

uint32_t MIDI_USB_GetPacketsCount(void)
{
    return midi_usb_packets;
}

uint32_t MIDI_USB_GetEventsCount(void)
{
    return midi_events;
}

uint32_t MIDI_USB_GetReceiveErrorsCount(void)
{
    return midi_receive_errors;
}
