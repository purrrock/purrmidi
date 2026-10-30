/**
 * @file    audio_halves.h
 * @brief   Планировщик заполнения двух половин циклического DMA-буфера звука.
 *
 * DMA-прерывание только отмечает, какая половина освободилась
 * (AudioHalves_Request), и запускает PendSV; рендер выполняется в PendSV с самым низким приоритетом
 * (AudioHalves_Next / AudioHalves_Done) и прерывается любым другим прерыванием.
 *
 * Временная диаграмма (P = период половины, 2,67 мс):
 *   Request(0) в момент T   - DMA играет половину 1, половина 0 свободна; её надо заполнить до T + P.
 *   Request(1) в момент T+P - DMA перешёл на половину 0 (!), половина 1 свободна.
 *   Если в момент Request(1) половина 0 ещё не заполнена (флаг pending), крайний срок пропущен: DMA уже играет
 *   недозаполненную половину 0 - это «опоздание» (late), оно считается и сообщается пользователю.
 *
 * Модуль не обращается к железу и состоит из двух сторон:
 *   - Request(): только из прерывания DMA (один писатель, самый высокий приоритет из двух сторон);
 *   - Next()/Done(): только из PendSV (один читатель).
 * Синхронизация - атомарные операции с acquire/release, критические секции не нужны.
 */
#ifndef AUDIO_HALVES_H
#define AUDIO_HALVES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t pending;    /* бит 0 - половина 0 ждёт заполнения, бит 1 - половина 1 */
    volatile uint32_t last_req;   /* какая половина запрошена последней (0/1)               */
    volatile uint32_t late;       /* сколько раз крайний срок был пропущен                  */
} AudioHalves;

void AudioHalves_Init(AudioHalves *h);

/**
 * Прерывание DMA: половина `half` (0 или 1) освободилась и ждёт новых данных.
 * @return true, если предыдущий запрос ещё не выполнен (опоздание: DMA уже играет недозаполненную половину).
 */
bool AudioHalves_Request(AudioHalves *h, unsigned half);

/**
 * PendSV: выбрать следующую половину для обработки.
 * @param half   [out] номер половины (0/1).
 * @param stale  [out] true - эту половину рендерить НЕ нужно, а залить тишиной: ждут обе половины, значит
 *               более старая уже пропустила свой срок и DMA её проигрывает (повтор старого звука хуже тишины).
 * @return false, если делать нечего.
 */
bool AudioHalves_Next(AudioHalves *h, unsigned *half, bool *stale);

/** PendSV: половина `half` обработана (отрендерена или залита тишиной). */
void AudioHalves_Done(AudioHalves *h, unsigned half);

/** Счётчик пропущенных сроков (для статистики и индикации на дисплее). */
uint32_t AudioHalves_LateCount(const AudioHalves *h);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_HALVES_H */
