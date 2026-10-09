#include "usb_port_recover.h"
#include "usbh_conf.h"

static uint32_t s_recover_cnt = 0U;

uint32_t USB_PortLostRecoverCount(void)
{
  return s_recover_cnt;
}

/*
 * Problem: some devices drop and re-assert their D+ pull-up shortly after the host
 * port reset. The OTG core sees DISCINT + PCDET and clears HPRT0.PENA - the port is
 * dead (no SOF, no SETUP). The stock event logic misses it (the Connect callback
 * overwrites is_disconnected, or the Disconnect callback is skipped because PCSTS is
 * already 1 again), so the first control request waits forever without any error.
 *
 * Detection: stack believes a device is connected and is past the reset phase, but
 * HPRT0.PENA is 0.
 *
 * Recovery: use the stock "disconnect + re-enumerate" path of USBH_Process():
 * is_disconnected -> HOST_DEV_DISCONNECTED (class DeInit, DeInitStateMachine, user
 * callback HOST_USER_DISCONNECTION) and is_ReEnumerated -> USBH_Start() instead of a
 * plain LL start. is_connected is left untouched, so HOST_IDLE runs the normal
 * "connected -> port reset -> enumeration" sequence again.
 *
 * Only public fields/functions of the host library are used.
 */
void USB_PortLostRecover(USBH_HandleTypeDef *phost)
{
  const HCD_HandleTypeDef *hhcd = (const HCD_HandleTypeDef *)phost->pData;
  uint32_t hprt0;

  if ((hhcd == NULL) ||
      (phost->device.is_connected == 0U) ||
      (phost->device.is_disconnected != 0U) ||
      (phost->device.is_ReEnumerated != 0U) ||
      (phost->gState < HOST_ENUMERATION) ||
      (phost->gState > HOST_CLASS))
  {
    return;
  }

  hprt0 = *(__IO uint32_t *)((uint32_t)hhcd->Instance + USB_OTG_HOST_PORT_BASE);
  if ((hprt0 & USB_OTG_HPRT_PENA) != 0U)
  {
    return;
  }

  s_recover_cnt++;
  USBH_UsrLog("USB port disabled by HW (HPRT0=0x%08lX) in state %u, restarting (#%lu)",
              (unsigned long)hprt0, (unsigned int)phost->gState,
              (unsigned long)s_recover_cnt);

  phost->device.is_ReEnumerated = 1U;
  phost->device.is_disconnected = 1U;
  (void)USBH_Stop(phost);   /* stop HCD, halt stuck channels, free control pipes */

  /* give the device time to finish its own re-attach before the next port reset */
  HAL_Delay(300U);
}
