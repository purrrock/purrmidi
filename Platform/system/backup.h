/**
 * @file    backup.h
 * @brief   Хранение настроек и записи о последнем сбое в резервных регистрах RTC (домен VBAT).
 *
 * Хранилище - 32 резервных регистра RTC (RTC->BKP0R..BKP31R, 128 байт). Они питаются от VDD, а при его пропадании - от
 * вывода VBAT (на плате WeAct: разъём P2, пин 6 "VB", батарейка CR2032 между VB и GND). Если батарейки нет, регистры
 * переживают только программные сбросы и сброс по IWDG/NRST, а после выключения питания они пусты или содержат мусор.
 * Поэтому ВСЕ данные защищены magic + версией + CRC32: при несовпадении блок считается пустым, и прибор работает с
 * настройками по умолчанию (в UART выводится сообщение). Батарейка не обязательна.
 *
 * Почему регистры RTC, а не backup SRAM (4 КБ): у backup SRAM нужно отдельно включать стабилизатор удержания (лишние
 * микроамперы от батарейки), а чтение неинициализированной памяти с ECC опасно (см. ram_init.c). Резервные регистры -
 * обычные триггеры без ECC, читаются в любой момент, и 128 байт хватает на настройки и на запись о сбое.
 *
 * ВАЖНО для CubeMX: НЕ включайте в PurrMidi.ioc периферию RTC с другим источником тактирования. HAL при смене источника
 * тактирования RTC делает сброс backup-домена (BDRST), и сохранённые данные пропадут при каждой загрузке.
 * Этому модулю CubeMX-настройки не нужны: он сам разрешает доступ к backup-домену (PWR_CR1.DBP) и тактирует APB-интерфейс RTC.
 *
 * Вызывается из основного контекста, кроме Backup_RecordFault() (работает из обработчиков сбоев, только регистры).
 */
#ifndef BACKUP_H
#define BACKUP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKUP_UNAVAILABLE = 0,   /* регистры недоступны (проверка записи-чтения не прошла): работа без хранилища */
    BACKUP_EMPTY       = 1,   /* валидных данных нет (первое включение, нет батарейки, разряжена, повреждены) */
    BACKUP_RESTORED    = 2    /* найден валидный блок, настройки можно применять */
} BackupState;

/** Счётчики, переживающие перезагрузки (и выключение питания при наличии батарейки). */
typedef struct {
    uint32_t boot_count;        /* число загрузок с момента, когда блок был создан */
    uint32_t wdg_reset_count;   /* сколько раз сброс был по IWDG1/WWDG1 */
    uint32_t fault_count;       /* сколько раз прибор падал в Hard/Mem/Bus/UsageFault или Error_Handler */
} BackupStats;

typedef struct {
    uint32_t kind;              /* 0 Hard, 1 Mem, 2 Bus, 3 Usage, 4 Error_Handler */
    uint32_t pc, lr, cfsr, hfsr;
    uint32_t uptime_ms;         /* HAL_GetTick() на момент сбоя */
} BackupFault;

/**
 * Разрешить доступ к backup-домену, проверить запись/чтение, проверить блок. Если блок невалиден - создать пустой.
 * Вызывать один раз после SystemClock_Config() (нужен LSI от IWDG только как запасной вариант тактирования).
 * Печатает в UART одну строку о состоянии (поэтому вызывать после инициализации UART).
 */
BackupState Backup_Init(void);

BackupState Backup_GetState(void);

/**
 * Учёт загрузки: увеличивает счётчики и запоминает флаги RCC->RSR. rsr - значение RSR до очистки (Watchdog_Start()
 * его запоминает). Вызывать один раз за загрузку, после Backup_Init().
 */
void Backup_NoteReset(uint32_t rsr);

/** Настройки: программа (номер Program Change, 0..65535) и громкость 0..127. Возвращает 1, если в блоке есть настройки. */
int  Backup_LoadSettings(uint16_t *program, uint8_t *volume);
void Backup_SaveSettings(uint16_t program, uint8_t volume);

/** Счётчики. При состоянии UNAVAILABLE возвращает нули. */
void Backup_GetStats(BackupStats *out);

/**
 * Запись о сбое. Backup_RecordFault() можно вызывать из обработчиков сбоев и Error_Handler: только регистры, без HAL,
 * printf и malloc. Backup_TakeFault() отдаёт запись, ещё не показанную пользователю, и помечает её показанной
 * (возвращает 1, если новая запись была).
 */
void Backup_RecordFault(uint32_t kind, uint32_t pc, uint32_t lr, uint32_t cfsr, uint32_t hfsr);
int  Backup_TakeFault(BackupFault *out);

/** Напечатать в UART счётчики и (один раз) запись о предыдущем сбое, если она есть. Вызывать после Backup_NoteReset(). */
void Backup_ReportPrevious(void);

#ifdef __cplusplus
}
#endif

#endif /* BACKUP_H */
