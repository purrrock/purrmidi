#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file    display.h
 * @brief   Интерфейс пользовательского интерфейса (UI) PurrMidi (дисплей 160x80 px).
 *
 * Единый экран без переключений режимов:
 *   y  0..13  Шапка: индикатор USB, имя клавиатуры, сустейн (SUS), счётчик голосов (used/max)
 *             (при возникновении ошибки — красная плашка, например "! E03 MIDI QUEUE FULL")
 *   y 16..31  Номер пресета и название текущего инструмента
 *   y 34..65  Крупное наименование ноты или аккорда (+ номер ноты и velocity)
 *   y 67..79  Громкость: шкала и значение в процентах
 *
 * Все функции Display_* вызываются ТОЛЬКО из главного цикла (не из прерываний ISR).
 * Функции-сеттеры лишь обновляют внутреннее состояние и помечают поля «грязными»;
 * фактическая отрисовка выполняется в Display_Task().
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    DISPLAY_LINK_NONE = 0,      /* Клавиатура не подключена  (серая точка)  */
    DISPLAY_LINK_ENUMERATING,   /* Выполняется энумерация   (жёлтая точка) */
    DISPLAY_LINK_READY          /* Клавиатура готова         (зелёная точка)*/
} Display_Link_t;

/* Коды ошибок (отображаются в виде красной плашки в шапке) */
typedef enum
{
    DISPLAY_ERR_NONE = 0,
    DISPLAY_ERR_USB_ENUM,       /* E01 USB ENUM FAIL     — энумерация > 5 с; до смены состояния USB-линка */
    DISPLAY_ERR_AUDIO_DMA,      /* E02 AUDIO DMA ERROR   — ошибка SAI/DMA; гаснет автоматически через 3 с */
    DISPLAY_ERR_MIDI_QUEUE,     /* E03 MIDI QUEUE FULL   — переполнение очереди; гаснет через 3 с       */
    DISPLAY_ERR_USB_RX,         /* E04 USB RX ERROR      — ошибка приёма USB; гаснет через 3 с          */
    DISPLAY_ERR_AUDIO_OVERLOAD, /* E05 AUDIO OVERLOAD    — пропуск аудио-блоков (load shedding); 3 с    */
    DISPLAY_ERR_SF2,            /* E06 SF2 NOT LOADED    — выбран SoundFont, а загрузка не удалась; 3 с  */
    DISPLAY_ERR_COUNT
} Display_Error_t;

void Display_Init(void);

/**
 * Экран прогресса при загрузке SoundFont-банка во флеш (до запуска основного UI).
 * text - до 20 символов ASCII, percent 0..100. Display_EndBootProgress() возвращает обычный интерфейс.
 */
void Display_ShowBootProgress(const char *text, uint8_t percent);
void Display_EndBootProgress(void);

/**
 * Вызывать из главного цикла как можно чаще: отрисовывает не более одного «грязного» поля за вызов
 * для предотвращения задержек в главном цикле.
 */
void Display_Task(void);

/* ---- Статус подключения ---- */
void Display_SetMidiConnected(bool connected);          /* Для совместимости со старым API */
void Display_SetLinkState(Display_Link_t state);
void Display_SetDeviceName(const char *name);           /* До 12 символов; NULL -> "USB MIDI" */

/* ---- Отображение нот и аккордов ---- */
void Display_SetLastNote(uint8_t note);                 /* Для совместимости со старым API */
void Display_SetNoteState(
    const uint8_t *active_notes,
    uint16_t active_note_count,
    uint8_t last_note);
void Display_SetVelocity(uint8_t velocity);             /* 0 = не показывать значение velocity */
void Display_SetSustain(bool on);

/* ---- Выбранный патч и громкость ---- */
void Display_SetPatch(uint8_t index, const char *name); /* Имя инструмента до 17 символов, только ASCII */
void Display_SetVoices(uint8_t used, uint8_t max);      /* max = 0 -> индикатор голосов скрыт */
void Display_SetVolume(uint8_t percent);                /* 0..100; пока не вызвана — показывает "--%" */
void Display_SetClip(bool clipping);                    /* Индикатор перегрузки: шкала громкости краснеет */

/* ---- Обработка ошибок ---- */
void Display_ShowError(Display_Error_t code);
void Display_ClearError(Display_Error_t code);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
