#ifndef USB_PORT_RECOVER_H
#define USB_PORT_RECOVER_H

#include <stdint.h>
#include "usbh_core.h"

/**
 * @file    usb_port_recover.h
 * @brief   Автоматическое восстановление USB-порта при спонтанном аппаратно отключении.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Функция восстановления порта USB Host при некорректном сбросе подтяжки D+ клавиатурой.
 * Вызывается периодически из главного цикла.
 */
void USB_PortLostRecover(USBH_HandleTypeDef *phost);

/** Возвращает суммарное количество проведенных процедур восстановления порта. */
uint32_t USB_PortLostRecoverCount(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_PORT_RECOVER_H */
