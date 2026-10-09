#ifndef USB_PORT_RECOVER_H
#define USB_PORT_RECOVER_H

#include <stdint.h>
#include "usbh_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Recover from a USB host port that was disabled by hardware while the device
 * is still attached (e.g. SMK25Mini drops D+ right after the port reset).
 *
 * Call it periodically from the main loop (after MX_USB_HOST_Process()).
 * Lives outside Middlewares/ on purpose: CubeMX overwrites usbh_core.c on every
 * code generation, application code is kept.
 */
void USB_PortLostRecover(USBH_HandleTypeDef *phost);

/** Number of recoveries performed since reset (for diagnostics). */
uint32_t USB_PortLostRecoverCount(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_PORT_RECOVER_H */
