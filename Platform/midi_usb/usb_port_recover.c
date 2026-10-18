#include "usb_port_recover.h"
#include "usbh_conf.h"

/* Пауза между остановкой порта и повторной перечислением устройства. Раньше здесь был
 * HAL_Delay(300), который блокировал главный цикл (приём MIDI, дисплей, кнопки) на 300 мс.
 * Теперь пауза неблокирующая: главный цикл лишь не вызывает стек USB Host, пока она идёт. */
#define USB_RECOVER_HOLD_MS 300U

static uint32_t s_recover_cnt   = 0U;
static uint32_t s_hold_start_ms = 0U;
static uint8_t  s_hold_active   = 0U;

uint32_t USB_PortLostRecoverCount(void)
{
  return s_recover_cnt;
}

uint8_t USB_PortLostRecoverPending(void)
{
  if (s_hold_active != 0U)
  {
    if ((HAL_GetTick() - s_hold_start_ms) >= USB_RECOVER_HOLD_MS)
    {
      s_hold_active = 0U;   /* пауза закончилась: стек USB снова можно обслуживать */
    }
  }
  return s_hold_active;
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

  /* Даём устройству время завершить собственное переподключение перед следующим сбросом порта.
   * Ждём без блокировки: см. USB_PortLostRecoverPending(). */
  s_hold_start_ms = HAL_GetTick();
  s_hold_active   = 1U;
}
