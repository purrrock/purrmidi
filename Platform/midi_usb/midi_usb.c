#include "midi_usb.h"
#include "main.h"
#include "usb_host.h"
#include "usbh_midi.h"
#include "midi_event.h"
#include "midi_queue.h"
#include "usb_midi_packet.h"

extern ApplicationTypeDef Appli_state;
extern USBH_HandleTypeDef hUsbHostFS;

__ALIGN_BEGIN static uint8_t midi_rx_buffer[64] __ALIGN_END;

static volatile uint32_t midi_receive_errors = 0;

static ApplicationTypeDef previous_state = APPLICATION_IDLE;
static bool state_changed = false;

void MIDI_USB_Init(void)
{
    midi_receive_errors = 0;
    previous_state = APPLICATION_IDLE;
    state_changed = false;
}

void USBH_MIDI_ReceiveCallback(USBH_HandleTypeDef *phost)
{
    const uint16_t length = USBH_MIDI_GetLastReceivedDataSize(phost);

    for (uint16_t i = 0; i + 3 < length; i += 4)
    {
        MIDI_Event_t event;

        /* SysEx / System Common / Real-Time / пустые пакеты синтезатору не нужны и отбрасываются. */
        if (UsbMidi_PacketToEvent(&midi_rx_buffer[i], &event))
        {
            MIDI_Queue_Push(&event);
        }
    }
}

void MIDI_USB_Process(void)
{
    if (Appli_state != previous_state)
    {
        state_changed = true;
        previous_state = Appli_state;
    }
    else
    {
        state_changed = false;
    }

    if (Appli_state == APPLICATION_READY)
    {
        const USBH_StatusTypeDef res = USBH_MIDI_Receive(&hUsbHostFS,
                                                         midi_rx_buffer,
                                                         sizeof(midi_rx_buffer));
        if (res != USBH_OK && res != USBH_BUSY)
        {
            midi_receive_errors++;
        }
    }
}

bool MIDI_USB_IsConnected(void)
{
    return (Appli_state == APPLICATION_READY);
}

bool MIDI_USB_HasStateChanged(void)
{
    return state_changed;
}

uint32_t MIDI_USB_GetReceiveErrorsCount(void)
{
    return midi_receive_errors;
}
