#ifndef USB_PORT_RECOVER_H
#define USB_PORT_RECOVER_H

#include <stdint.h>
#include "usbh_core.h"

/**
 * @file    usb_port_recover.h
 * @brief   Восстановление USB-порта Host при аппаратно отключении.
 *
 * Восстановление порта USB Host, который был автоматически отключён аппаратной периферией,
 * когда устройство всё ещё подключено (например, клавиатура SMK25Mini сбрасывает подтяжку D+
 * сразу после сброса порта).
 *
 * Вызывается периодически из главного цикла (после вызова MX_USB_HOST_Process()).
 * Расположен вне каталога Middlewares/ намеренно: CubeMX перезаписывает usbh_core.c при каждой
 * генерации кода проекта, тогда как пользовательский код приложения за пределами Middlewares/ сохраняется.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Функция восстановления порта USB Host при некорректном сбросе подтяжки D+ клавиатурой.
 * Вызывается периодически из главного цикла.
 */
void USB_PortLostRecover(USBH_HandleTypeDef *phost);

/** Возвращает суммарное количество проведенных процедур восстановления порта (для диагностики). */
uint32_t USB_PortLostRecoverCount(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_PORT_RECOVER_H */
