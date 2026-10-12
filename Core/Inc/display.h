#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PurrMidi UI, 160x80, один экран без переключений:
 *
 *   y  0..13  шапка: точка статуса USB, имя клавиатуры, SUS, V used/max
 *             (при ошибке - красная плашка "! E03 MIDI QUEUE FULL")
 *   y 16..31  номер пресета и название инструмента
 *   y 34..65  нота / аккорд крупно (+ номер ноты и velocity)
 *   y 67..79  громкость: полоса и проценты
 *
 * Все функции Display_* вызываются ТОЛЬКО из главного цикла (не из ISR).
 * Сеттеры лишь обновляют состояние и помечают поля "грязными";
 * реальная отрисовка происходит в Display_Task().
 */

typedef enum
{
    DISPLAY_LINK_NONE = 0,      /* клавиатуры нет            (серая точка)  */
    DISPLAY_LINK_ENUMERATING,   /* идёт энумерация           (жёлтая точка) */
    DISPLAY_LINK_READY          /* клавиатура готова         (зелёная точка)*/
} Display_Link_t;

/* Коды ошибок (красная плашка в шапке) */
typedef enum
{
    DISPLAY_ERR_NONE = 0,
    DISPLAY_ERR_USB_ENUM,       /* E01 USB ENUM FAIL     - энумерация > 5 с; до смены состояния USB-линка */
    DISPLAY_ERR_AUDIO_DMA,      /* E02 AUDIO DMA ERROR   - ошибка SAI/DMA;        гаснет через 3 с       */
    DISPLAY_ERR_MIDI_QUEUE,     /* E03 MIDI QUEUE FULL   - переполнение очереди;  гаснет через 3 с       */
    DISPLAY_ERR_USB_RX,         /* E04 USB RX ERROR      - ошибка приёма USB;     гаснет через 3 с       */
    DISPLAY_ERR_AUDIO_OVERLOAD, /* E05 AUDIO OVERLOAD    - блоки заменены тишиной (load shedding); 3 с   */
    DISPLAY_ERR_SF2,            /* E06 SF2 NOT LOADED    - выбран SoundFont, а загрузка не удалась; 3 с  */
    DISPLAY_ERR_COUNT
} Display_Error_t;

void Display_Init(void);

/* Вызывать из главного цикла как можно чаще: рисует не более одного поля за вызов. */
void Display_Task(void);

/* ---- Подключение ---- */
void Display_SetMidiConnected(bool connected);          /* совместимость со старым API */
void Display_SetLinkState(Display_Link_t state);
void Display_SetDeviceName(const char *name);           /* до 12 символов; NULL -> "USB MIDI" */

/* ---- Ноты ---- */
void Display_SetLastNote(uint8_t note);                 /* совместимость со старым API */
void Display_SetNoteState(
    const uint8_t *active_notes,
    uint16_t active_note_count,
    uint8_t last_note);
void Display_SetVelocity(uint8_t velocity);             /* 0 = не показывать */
void Display_SetSustain(bool on);

/* ---- Инструмент и громкость ---- */
void Display_SetPatch(uint8_t index, const char *name); /* имя до 17 символов, только ASCII */
void Display_SetVoices(uint8_t used, uint8_t max);      /* max = 0 -> индикатор скрыт */
void Display_SetVolume(uint8_t percent);                /* 0..100; пока не вызвана - "--%" */
void Display_SetClip(bool clipping);                    /* полоса громкости краснеет */

/* ---- Ошибки ---- */
void Display_ShowError(Display_Error_t code);
void Display_ClearError(Display_Error_t code);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
