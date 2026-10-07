/**
  ******************************************************************************
  * @file    stm32h7xx_hal_hcd.c
  * @author  MCD Application Team
  * @brief   HCD HAL module driver.
  *          This file provides firmware functions to manage the following
  *          functionalities of the USB Peripheral Controller:
  *           + Initialization and de-initialization functions
  *           + IO operation functions
  *           + Peripheral Control functions
  *           + Peripheral State functions
  *
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2017 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  @verbatim
  ==============================================================================
                    ##### How to use this driver #####
  ==============================================================================
  [..]
    (#)Declare a HCD_HandleTypeDef handle structure, for example:
       HCD_HandleTypeDef  hhcd;

    (#)Fill parameters of Init structure in HCD handle

    (#)Call HAL_HCD_Init() API to initialize the HCD peripheral (Core, Host core, ...)

    (#)Initialize the HCD low level resources through the HAL_HCD_MspInit() API:
        (##) Enable the HCD/USB Low Level interface clock using the following macros
             (+++) __HAL_RCC_USB_OTG_FS_CLK_ENABLE();
             (+++) __HAL_RCC_USB_OTG_HS_CLK_ENABLE(); (For High Speed Mode)
             (+++) __HAL_RCC_USB_OTG_HS_ULPI_CLK_ENABLE(); (For High Speed Mode)

        (##) Initialize the related GPIO clocks
        (##) Configure HCD pin-out
        (##) Configure HCD NVIC interrupt

    (#)Associate the Upper USB Host stack to the HAL HCD Driver:
        (##) hhcd.pData = phost;

    (#)Enable HCD transmission and reception:
        (##) HAL_HCD_Start();

  @endverbatim
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"
#include <stdio.h>

/* Minimal HCD Diagnostic Helpers and Rate Limiting */
typedef struct {
  uint32_t last_hcint;
  uint32_t last_hcintmsk;
  uint32_t last_urb_state;
  uint32_t last_hc_state;
  uint32_t last_log_tick;
  uint32_t repeat_count;
} HCD_ChannelLogState;

typedef struct {
  uint32_t last_gintsts;
  uint32_t last_gintmsk;
  uint32_t last_haint;
  uint32_t last_haintmsk;
  uint32_t last_log_tick;
} HCD_GintLogState;

typedef struct {
  uint32_t last_bitmap;
  uint8_t initialized;
} HCD_HaintLogState;

typedef struct {
  uint32_t last_gintsts;
  uint32_t last_haint;
  uint32_t last_haintmsk;
  uint32_t last_hcint;
  uint32_t last_hcintmsk;
  uint32_t last_hcchar;
  uint32_t last_hctsiz;
  uint32_t last_log_tick;
} HCD_DispatchLogState;

typedef struct {
  uint32_t last_raw_hcint;
  uint32_t last_cause_hcint;
  uint32_t last_hcintmsk;
  uint32_t last_hcchar;
  uint32_t last_hctsiz;
  uint32_t last_haint;
  uint32_t last_haintmsk;
  uint32_t last_gintsts;
  uint32_t last_gintmsk;
  uint32_t last_hfnum;
  uint32_t last_hprt0;
  uint32_t last_hc_state;
  uint32_t last_urb_state;
  uint32_t last_xfer_count;
  uint32_t last_toggle_in;
  uint32_t last_log_tick;
  uint32_t repeat_count;
} HCD_NotReadyLogState;

static HCD_ChannelLogState s_irq_log_state[16];
static HCD_ChannelLogState s_hc_log_state[16];
static HCD_ChannelLogState s_chh_log_state[16];
static HCD_GintLogState s_gint_log_state;
static HCD_HaintLogState s_haint_log_state;
static HCD_DispatchLogState s_dispatch_log_state[16];
static HCD_NotReadyLogState s_notready_log_state[16];
static uint32_t s_last_cause_hcint[16];
static uint8_t s_force_ch0_dispatch_log = 1U;

static const char *HCD_HCStateStr(uint32_t state)
{
  switch (state) {
    case HC_IDLE:       return "IDLE";
    case HC_XFRC:       return "XFRC";
    case HC_HALTED:     return "HALTED";
    case HC_ACK:        return "ACK";
    case HC_NAK:        return "NAK";
    case HC_NYET:       return "NYET";
    case HC_STALL:      return "STALL";
    case HC_XACTERR:    return "XACTERR";
    case HC_BBLERR:     return "BBLERR";
    case HC_DATATGLERR: return "DATATGLERR";
    default:            return "UNKNOWN";
  }
}

static const char *HCD_URBStateStr(uint32_t urb)
{
  switch (urb) {
    case URB_IDLE:     return "IDLE";
    case URB_DONE:     return "DONE";
    case URB_NOTREADY: return "NOTREADY";
    case URB_NYET:     return "NYET";
    case URB_ERROR:    return "ERROR";
    case URB_STALL:    return "STALL";
    case URB_NAK_WAIT: return "NAK_WAIT";
    default:           return "UNKNOWN";
  }
}

static const char *HCD_EPTypeStr(uint32_t type)
{
  switch (type) {
    case EP_TYPE_CTRL: return "CTRL";
    case EP_TYPE_ISOC: return "ISOC";
    case EP_TYPE_BULK: return "BULK";
    case EP_TYPE_INTR: return "INTR";
    default:           return "UNKNOWN";
  }
}

static uint8_t HCD_ShouldLog(HCD_ChannelLogState *st, uint32_t hcint, uint32_t hcintmsk, uint32_t urb_state, uint32_t hc_state)
{
  uint32_t now = HAL_GetTick();
  uint8_t changed = 0;

  if (st->last_hcint != hcint || st->last_hcintmsk != hcintmsk ||
      st->last_urb_state != urb_state || st->last_hc_state != hc_state)
  {
    changed = 1;
  }

  if (changed || (now - st->last_log_tick >= 1000U))
  {
    st->last_hcint = hcint;
    st->last_hcintmsk = hcintmsk;
    st->last_urb_state = urb_state;
    st->last_hc_state = hc_state;
    st->last_log_tick = now;
    return 1;
  }

  st->repeat_count++;
  return 0;
}

/* Diagnostic #1 Logger */
static void HCD_LogIRQ(HCD_HandleTypeDef *hhcd, uint8_t chnum)
{
  const USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
  uint32_t USBx_BASE = (uint32_t)USBx;
  uint32_t hcchar = USBx_HC(chnum)->HCCHAR;
  uint32_t hcint = USBx_HC(chnum)->HCINT;
  uint32_t hcintmsk = USBx_HC(chnum)->HCINTMSK;
  uint32_t hctsiz = USBx_HC(chnum)->HCTSIZ;
  uint32_t hcsplt = USBx_HC(chnum)->HCSPLT;
  uint32_t state = (uint32_t)hhcd->hc[chnum].state;
  uint32_t urb = (uint32_t)hhcd->hc[chnum].urb_state;

  if (urb == URB_IDLE && state == HC_IDLE)
  {
    return;
  }

  if (HCD_ShouldLog(&s_irq_log_state[chnum], hcint, hcintmsk, urb, state))
  {
    uint32_t rpt = s_irq_log_state[chnum].repeat_count;
    s_irq_log_state[chnum].repeat_count = 0;

    printf("[HCDIRQ] ch=%u HCINT=0x%03lX MSK=0x%03lX HCCHAR=0x%08lX HCTSIZ=0x%08lX HCSPLT=0x%08lX state=%s urb=%s ep=%u dir=%s type=%s%s\n",
           (unsigned int)chnum,
           (unsigned long)hcint,
           (unsigned long)hcintmsk,
           (unsigned long)hcchar,
           (unsigned long)hctsiz,
           (unsigned long)hcsplt,
           HCD_HCStateStr(state),
           HCD_URBStateStr(urb),
           (unsigned int)hhcd->hc[chnum].ep_num,
           ((hcchar & USB_OTG_HCCHAR_EPDIR) == USB_OTG_HCCHAR_EPDIR) ? "IN" : "OUT",
           HCD_EPTypeStr((uint32_t)hhcd->hc[chnum].ep_type),
           (rpt > 0) ? " (rpt)" : "");
  }
}

/* Diagnostic NOTREADY Logger */
static void HCD_LogNotReady(HCD_HandleTypeDef *hhcd, uint8_t chnum, uint32_t raw_hcint, uint32_t raw_hcintmsk)
{
  const USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
  uint32_t USBx_BASE = (uint32_t)USBx;
  uint32_t hcchar = USBx_HC(chnum)->HCCHAR;
  uint32_t hctsiz = USBx_HC(chnum)->HCTSIZ;
  uint32_t haint = USBx_HOST->HAINT;
  uint32_t haintmsk = USBx_HOST->HAINTMSK;
  uint32_t gintsts = USBx->GINTSTS;
  uint32_t gintmsk = USBx->GINTMSK;
  uint32_t hfnum = USBx_HOST->HFNUM;
  uint32_t hprt0 = USBx_HPRT0;
  uint32_t hc_state = (uint32_t)hhcd->hc[chnum].state;
  uint32_t hc_urb_state = (uint32_t)hhcd->hc[chnum].urb_state;
  uint32_t xfer_count = hhcd->hc[chnum].xfer_count;
  uint32_t toggle_in = (uint32_t)hhcd->hc[chnum].toggle_in;
  uint32_t cause_hcint = s_last_cause_hcint[chnum];
  uint32_t now = HAL_GetTick();

  HCD_NotReadyLogState *st = &s_notready_log_state[chnum];

  uint8_t changed = (st->last_raw_hcint != raw_hcint ||
                     st->last_cause_hcint != cause_hcint ||
                     st->last_hcintmsk != raw_hcintmsk ||
                     st->last_hcchar != hcchar ||
                     st->last_hctsiz != hctsiz ||
                     st->last_haint != haint ||
                     st->last_haintmsk != haintmsk ||
                     st->last_gintsts != gintsts ||
                     st->last_gintmsk != gintmsk ||
                     st->last_hprt0 != hprt0 ||
                     st->last_hc_state != hc_state ||
                     st->last_urb_state != hc_urb_state ||
                     st->last_xfer_count != xfer_count ||
                     st->last_toggle_in != toggle_in);

  if (changed || (now - st->last_log_tick >= 1000U))
  {
    st->last_raw_hcint = raw_hcint;
    st->last_cause_hcint = cause_hcint;
    st->last_hcintmsk = raw_hcintmsk;
    st->last_hcchar = hcchar;
    st->last_hctsiz = hctsiz;
    st->last_haint = haint;
    st->last_haintmsk = haintmsk;
    st->last_gintsts = gintsts;
    st->last_gintmsk = gintmsk;
    st->last_hfnum = hfnum;
    st->last_hprt0 = hprt0;
    st->last_hc_state = hc_state;
    st->last_urb_state = hc_urb_state;
    st->last_xfer_count = xfer_count;
    st->last_toggle_in = toggle_in;
    st->last_log_tick = now;

    printf("[HCDNOTREADY] ch=%u HCINT=0x%08lX (cause=0x%08lX) HCINTMSK=0x%08lX HCCHAR=0x%08lX HCTSIZ=0x%08lX HAINT=0x%08lX HAINTMSK=0x%08lX GINTSTS=0x%08lX GINTMSK=0x%08lX HFNUM=0x%08lX HPRT0=0x%08lX hc.state=%lu(%s) hc.urb_state=%lu(%s) hc.xfer_count=%lu hc.toggle_in=%lu\n",
           (unsigned int)chnum,
           (unsigned long)raw_hcint,
           (unsigned long)cause_hcint,
           (unsigned long)raw_hcintmsk,
           (unsigned long)hcchar,
           (unsigned long)hctsiz,
           (unsigned long)haint,
           (unsigned long)haintmsk,
           (unsigned long)gintsts,
           (unsigned long)gintmsk,
           (unsigned long)hfnum,
           (unsigned long)hprt0,
           (unsigned long)hc_state,
           HCD_HCStateStr(hc_state),
           (unsigned long)hc_urb_state,
           HCD_URBStateStr(hc_urb_state),
           (unsigned long)xfer_count,
           (unsigned long)toggle_in);
  }
}

/* Diagnostic GINT Logger */
static void HCD_LogGINT(HCD_HandleTypeDef *hhcd)
{
  const USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
  uint32_t USBx_BASE = (uint32_t)USBx;
  uint32_t gintsts = USBx->GINTSTS;
  uint32_t gintmsk = USBx->GINTMSK;
  uint32_t haint = USBx_HOST->HAINT;
  uint32_t haintmsk = USBx_HOST->HAINTMSK;
  uint32_t now = HAL_GetTick();

  if (s_gint_log_state.last_gintsts != gintsts ||
      s_gint_log_state.last_gintmsk != gintmsk ||
      s_gint_log_state.last_haint != haint ||
      s_gint_log_state.last_haintmsk != haintmsk ||
      (now - s_gint_log_state.last_log_tick >= 100U))
  {
    s_gint_log_state.last_gintsts = gintsts;
    s_gint_log_state.last_gintmsk = gintmsk;
    s_gint_log_state.last_haint = haint;
    s_gint_log_state.last_haintmsk = haintmsk;
    s_gint_log_state.last_log_tick = now;

    printf("[HCDGINT] GINTSTS=0x%08lX GINTMSK=0x%08lX HAINT=0x%08lX HAINTMSK=0x%08lX HPRT0=0x%08lX HFNUM=0x%08lX\n",
           (unsigned long)gintsts,
           (unsigned long)gintmsk,
           (unsigned long)haint,
           (unsigned long)haintmsk,
           (unsigned long)USBx_HPRT0,
           (unsigned long)USBx_HOST->HFNUM);
  }
}

/* Diagnostic HAINT Bitmap Logger */
static void HCD_LogHAINT(uint32_t bitmap)
{
  if (!s_haint_log_state.initialized || s_haint_log_state.last_bitmap != bitmap)
  {
    uint32_t old_bitmap = s_haint_log_state.last_bitmap;
    s_haint_log_state.initialized = 1;
    s_haint_log_state.last_bitmap = bitmap;
    printf("[HCDHAINT] old=0x%08lX new=0x%08lX\n", (unsigned long)old_bitmap, (unsigned long)bitmap);
  }
}

/* Diagnostic Channel Dispatch Logger */
static void HCD_LogDispatch(HCD_HandleTypeDef *hhcd, uint8_t chnum)
{
  const USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
  uint32_t USBx_BASE = (uint32_t)USBx;
  uint32_t gintsts = USBx->GINTSTS;
  uint32_t haint = USBx_HOST->HAINT;
  uint32_t haintmsk = USBx_HOST->HAINTMSK;
  uint32_t hcint = USBx_HC(chnum)->HCINT;
  uint32_t hcintmsk = USBx_HC(chnum)->HCINTMSK;
  uint32_t hcchar = USBx_HC(chnum)->HCCHAR;
  uint32_t hctsiz = USBx_HC(chnum)->HCTSIZ;
  uint32_t now = HAL_GetTick();

  HCD_DispatchLogState *st = &s_dispatch_log_state[chnum];
  uint8_t force = (chnum == 0U && s_force_ch0_dispatch_log != 0U);

  uint8_t changed = (st->last_gintsts != gintsts ||
                     st->last_haint != haint ||
                     st->last_haintmsk != haintmsk ||
                     st->last_hcint != hcint ||
                     st->last_hcintmsk != hcintmsk ||
                     st->last_hcchar != hcchar ||
                     st->last_hctsiz != hctsiz);

  if (force || changed || (now - st->last_log_tick >= 1000U))
  {
    st->last_gintsts = gintsts;
    st->last_haint = haint;
    st->last_haintmsk = haintmsk;
    st->last_hcint = hcint;
    st->last_hcintmsk = hcintmsk;
    st->last_hcchar = hcchar;
    st->last_hctsiz = hctsiz;
    st->last_log_tick = now;

    if (chnum == 0U)
    {
      s_force_ch0_dispatch_log = 0U;
    }

    printf("[HCDDISPATCH] GINTSTS=0x%08lX HAINT=0x%08lX HAINTMSK=0x%08lX ch=%u HCINT=0x%03lX HCINTMSK=0x%03lX HCCHAR=0x%08lX HCTSIZ=0x%08lX\n",
           (unsigned long)gintsts,
           (unsigned long)haint,
           (unsigned long)haintmsk,
           (unsigned int)chnum,
           (unsigned long)(hcint & 0x7FFU),
           (unsigned long)(hcintmsk & 0x7FFU),
           (unsigned long)hcchar,
           (unsigned long)hctsiz);
  }
}

/* Diagnostic #2 Logger */
static void HCD_LogHC(HCD_HandleTypeDef *hhcd, uint8_t chnum, uint8_t is_in, uint32_t raw_hcint, uint32_t raw_hcintmsk)
{
  uint32_t state = (uint32_t)hhcd->hc[chnum].state;
  uint32_t urb = (uint32_t)hhcd->hc[chnum].urb_state;

  if (HCD_ShouldLog(&s_hc_log_state[chnum], raw_hcint, raw_hcintmsk, urb, state))
  {
    uint32_t rpt = s_hc_log_state[chnum].repeat_count;
    s_hc_log_state[chnum].repeat_count = 0;

    printf("[HCDHC] ch=%u dir=%s HCINT=0x%03lX MSK=0x%03lX flags: %s%s%s%s%s%s%s%s%s%s%s%s\n",
           (unsigned int)chnum,
           is_in ? "IN" : "OUT",
           (unsigned long)raw_hcint,
           (unsigned long)raw_hcintmsk,
           (raw_hcint & USB_OTG_HCINT_XFRC)   ? "XFRC "   : "",
           (raw_hcint & USB_OTG_HCINT_CHH)    ? "CHH "    : "",
           (raw_hcint & USB_OTG_HCINT_AHBERR) ? "AHBERR " : "",
           (raw_hcint & USB_OTG_HCINT_STALL)  ? "STALL "  : "",
           (raw_hcint & USB_OTG_HCINT_NAK)    ? "NAK "    : "",
           (raw_hcint & USB_OTG_HCINT_ACK)    ? "ACK "    : "",
           (raw_hcint & USB_OTG_HCINT_NYET)   ? "NYET "   : "",
           (raw_hcint & USB_OTG_HCINT_TXERR)  ? "TXERR "  : "",
           (raw_hcint & USB_OTG_HCINT_BBERR)  ? "BBERR "  : "",
           (raw_hcint & USB_OTG_HCINT_FRMOR)  ? "FRMOR "  : "",
           (raw_hcint & USB_OTG_HCINT_DTERR)  ? "DTERR "  : "",
           (rpt > 0) ? "(rpt)" : "");
  }
}

/* Diagnostic #3 Logger */
static void HCD_LogCHH(HCD_HandleTypeDef *hhcd, uint8_t chnum, uint8_t is_in, uint32_t prev_state, uint32_t raw_hcint, uint32_t raw_hcintmsk)
{
  const USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
  uint32_t USBx_BASE = (uint32_t)USBx;
  uint32_t hcchar = USBx_HC(chnum)->HCCHAR;
  uint32_t hctsiz = USBx_HC(chnum)->HCTSIZ;
  uint32_t state = (uint32_t)hhcd->hc[chnum].state;
  uint32_t urb = (uint32_t)hhcd->hc[chnum].urb_state;

  if (HCD_ShouldLog(&s_chh_log_state[chnum], raw_hcint, raw_hcintmsk, urb, state))
  {
    uint32_t rpt = s_chh_log_state[chnum].repeat_count;
    s_chh_log_state[chnum].repeat_count = 0;

    printf("[HCDCHH] ch=%u dir=%s ep=%u type=%s prev_state=%s state=%s urb=%s ErrCnt=%lu NakCnt=%lu NyetCnt=%lu HCINT=0x%03lX MSK=0x%03lX HCCHAR=0x%08lX HCTSIZ=0x%08lX xfer_len=%lu xfer_cnt=%lu mps=%u ssplit=%u csplit=%u%s\n",
           (unsigned int)chnum,
           is_in ? "IN" : "OUT",
           (unsigned int)hhcd->hc[chnum].ep_num,
           HCD_EPTypeStr((uint32_t)hhcd->hc[chnum].ep_type),
           HCD_HCStateStr(prev_state),
           HCD_HCStateStr(state),
           HCD_URBStateStr(urb),
           (unsigned long)hhcd->hc[chnum].ErrCnt,
           (unsigned long)hhcd->hc[chnum].NakCnt,
           (unsigned long)hhcd->hc[chnum].NyetErrCnt,
           (unsigned long)raw_hcint,
           (unsigned long)raw_hcintmsk,
           (unsigned long)hcchar,
           (unsigned long)hctsiz,
           (unsigned long)hhcd->hc[chnum].xfer_len,
           (unsigned long)hhcd->hc[chnum].xfer_count,
           (unsigned int)hhcd->hc[chnum].max_packet,
           (unsigned int)hhcd->hc[chnum].do_ssplit,
           (unsigned int)hhcd->hc[chnum].do_csplit,
           (rpt > 0) ? " (rpt)" : "");
  }
}

/** @addtogroup STM32H7xx_HAL_Driver
  * @{
  */

#ifdef HAL_HCD_MODULE_ENABLED
#if defined (USB_OTG_FS) || defined (USB_OTG_HS)

/** @defgroup HCD HCD
  * @brief HCD HAL module driver
  * @{
  */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/** @defgroup HCD_Private_Functions HCD Private Functions
  * @{
  */
static void HCD_HC_IN_IRQHandler(HCD_HandleTypeDef *hhcd, uint8_t chnum);
static void HCD_HC_OUT_IRQHandler(HCD_HandleTypeDef *hhcd, uint8_t chnum);
static void HCD_RXQLVL_IRQHandler(HCD_HandleTypeDef *hhcd);
static void HCD_Port_IRQHandler(HCD_HandleTypeDef *hhcd);
/**
  * @}
  */

/* Exported functions --------------------------------------------------------*/
/** @defgroup HCD_Exported_Functions HCD Exported Functions
  * @{
  */

/** @defgroup HCD_Exported_Functions_Group1 Initialization and de-initialization functions
  *  @brief    Initialization and Configuration functions
  *
@verbatim
 ===============================================================================
          ##### Initialization and de-initialization functions #####
 ===============================================================================
    [..]  This section provides functions allowing to:

@endverbatim
  * @{
  */

/**
  * @brief  Initialize the host driver.
  * @param  hhcd HCD handle
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_Init(HCD_HandleTypeDef *hhcd)
{
  /* Check the HCD handle allocation */
  if (hhcd == NULL)
  {
    return HAL_ERROR;
  }

  /* Check the parameters */
  assert_param(IS_HCD_ALL_INSTANCE(hhcd->Instance));

  if (hhcd->State == HAL_HCD_STATE_RESET)
  {
    /* Allocate lock resource and initialize it */
    hhcd->Lock = HAL_UNLOCKED;

#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
    hhcd->SOFCallback = HAL_HCD_SOF_Callback;
    hhcd->ConnectCallback = HAL_HCD_Connect_Callback;
    hhcd->DisconnectCallback = HAL_HCD_Disconnect_Callback;
    hhcd->PortEnabledCallback = HAL_HCD_PortEnabled_Callback;
    hhcd->PortDisabledCallback = HAL_HCD_PortDisabled_Callback;
    hhcd->HC_NotifyURBChangeCallback = HAL_HCD_HC_NotifyURBChange_Callback;

    if (hhcd->MspInitCallback == NULL)
    {
      hhcd->MspInitCallback = HAL_HCD_MspInit;
    }

    /* Init the low level hardware */
    hhcd->MspInitCallback(hhcd);
#else
    /* Init the low level hardware : GPIO, CLOCK, NVIC... */
    HAL_HCD_MspInit(hhcd);
#endif /* (USE_HAL_HCD_REGISTER_CALLBACKS) */
  }

  hhcd->State = HAL_HCD_STATE_BUSY;

  /* Disable the Interrupts */
  __HAL_HCD_DISABLE(hhcd);

  /* Init the Core (common init.) */
  if (USB_CoreInit(hhcd->Instance, hhcd->Init) != HAL_OK)
  {
    hhcd->State = HAL_HCD_STATE_ERROR;
    return HAL_ERROR;
  }

  /* Force Host Mode */
  if (USB_SetCurrentMode(hhcd->Instance, USB_HOST_MODE) != HAL_OK)
  {
    hhcd->State = HAL_HCD_STATE_ERROR;
    return HAL_ERROR;
  }

  /* Init Host */
  if (USB_HostInit(hhcd->Instance, hhcd->Init) != HAL_OK)
  {
    hhcd->State = HAL_HCD_STATE_ERROR;
    return HAL_ERROR;
  }

  hhcd->State = HAL_HCD_STATE_READY;

  return HAL_OK;
}

/**
  * @brief  Initialize a host channel.
  * @param  hhcd HCD handle
  * @param  ch_num Channel number.
  *         This parameter can be a value from 1 to 15
  * @param  epnum Endpoint number.
  *          This parameter can be a value from 1 to 15
  * @param  dev_address Current device address
  *          This parameter can be a value from 0 to 255
  * @param  speed Current device speed.
  *          This parameter can be one of these values:
  *            HCD_DEVICE_SPEED_HIGH: High speed mode,
  *            HCD_DEVICE_SPEED_FULL: Full speed mode,
  *            HCD_DEVICE_SPEED_LOW: Low speed mode
  * @param  ep_type Endpoint Type.
  *          This parameter can be one of these values:
  *            EP_TYPE_CTRL: Control type,
  *            EP_TYPE_ISOC: Isochronous type,
  *            EP_TYPE_BULK: Bulk type,
  *            EP_TYPE_INTR: Interrupt type
  * @param  mps Max Packet Size.
  *          This parameter can be a value from 0 to32K
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_HC_Init(HCD_HandleTypeDef *hhcd, uint8_t ch_num, uint8_t epnum,
                                  uint8_t dev_address, uint8_t speed, uint8_t ep_type, uint16_t mps)
{
  HAL_StatusTypeDef status;
  uint32_t HostCoreSpeed;
  uint32_t HCcharMps = mps;

  __HAL_LOCK(hhcd);
  hhcd->hc[ch_num].do_ping = 0U;
  hhcd->hc[ch_num].dev_addr = dev_address;
  hhcd->hc[ch_num].ch_num = ch_num;
  hhcd->hc[ch_num].ep_type = ep_type;
  hhcd->hc[ch_num].ep_num = epnum & 0x7FU;

  (void)HAL_HCD_HC_ClearHubInfo(hhcd, ch_num);

  if ((epnum & 0x80U) == 0x80U)
  {
    hhcd->hc[ch_num].ep_is_in = 1U;
  }
  else
  {
    hhcd->hc[ch_num].ep_is_in = 0U;
  }

  HostCoreSpeed = USB_GetHostSpeed(hhcd->Instance);

  if (ep_type == EP_TYPE_ISOC)
  {
    /* FS device plugged to HS HUB */
    if ((speed == HCD_DEVICE_SPEED_FULL) && (HostCoreSpeed == HPRT0_PRTSPD_HIGH_SPEED))
    {
      if (HCcharMps > ISO_SPLT_MPS)
      {
        /* ISO Max Packet Size for Split mode */
        HCcharMps = ISO_SPLT_MPS;
      }
    }
  }

  hhcd->hc[ch_num].speed = speed;
  hhcd->hc[ch_num].max_packet = (uint16_t)HCcharMps;

  status =  USB_HC_Init(hhcd->Instance, ch_num, epnum,
                        dev_address, speed, ep_type, (uint16_t)HCcharMps);

  __HAL_UNLOCK(hhcd);

  return status;
}

/**
  * @brief  Halt a host channel.
  * @param  hhcd HCD handle
  * @param  ch_num Channel number.
  *         This parameter can be a value from 1 to 15
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_HC_Halt(HCD_HandleTypeDef *hhcd, uint8_t ch_num)
{
  HAL_StatusTypeDef status = HAL_OK;

  __HAL_LOCK(hhcd);
  (void)USB_HC_Halt(hhcd->Instance, ch_num);
  __HAL_UNLOCK(hhcd);

  return status;
}

/**
  * @brief  DeInitialize the host driver.
  * @param  hhcd HCD handle
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_DeInit(HCD_HandleTypeDef *hhcd)
{
  /* Check the HCD handle allocation */
  if (hhcd == NULL)
  {
    return HAL_ERROR;
  }

  hhcd->State = HAL_HCD_STATE_BUSY;

#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
  if (hhcd->MspDeInitCallback == NULL)
  {
    hhcd->MspDeInitCallback = HAL_HCD_MspDeInit; /* Legacy weak MspDeInit  */
  }

  /* DeInit the low level hardware */
  hhcd->MspDeInitCallback(hhcd);
#else
  /* DeInit the low level hardware: CLOCK, NVIC.*/
  HAL_HCD_MspDeInit(hhcd);
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */

  __HAL_HCD_DISABLE(hhcd);

  hhcd->State = HAL_HCD_STATE_RESET;

  return HAL_OK;
}

/**
  * @brief  Initialize the HCD MSP.
  * @param  hhcd HCD handle
  * @retval None
  */
__weak void  HAL_HCD_MspInit(HCD_HandleTypeDef *hhcd)
{
  /* Prevent unused argument(s) compilation warning */
  UNUSED(hhcd);

  /* NOTE : This function should not be modified, when the callback is needed,
            the HAL_HCD_MspInit could be implemented in the user file
   */
}

/**
  * @brief  DeInitialize the HCD MSP.
  * @param  hhcd HCD handle
  * @retval None
  */
__weak void  HAL_HCD_MspDeInit(HCD_HandleTypeDef *hhcd)
{
  /* Prevent unused argument(s) compilation warning */
  UNUSED(hhcd);

  /* NOTE : This function should not be modified, when the callback is needed,
            the HAL_HCD_MspDeInit could be implemented in the user file
   */
}

/**
  * @}
  */

/** @defgroup HCD_Exported_Functions_Group2 Input and Output operation functions
  *  @brief   HCD IO operation functions
  *
@verbatim
 ===============================================================================
                      ##### IO operation functions #####
 ===============================================================================
 [..] This subsection provides a set of functions allowing to manage the USB Host Data
    Transfer

@endverbatim
  * @{
  */

/**
  * @brief  Submit a new URB for processing.
  * @param  hhcd HCD handle
  * @param  ch_num Channel number.
  *         This parameter can be a value from 1 to 15
  * @param  direction Channel number.
  *          This parameter can be one of these values:
  *           0 : Output / 1 : Input
  * @param  ep_type Endpoint Type.
  *          This parameter can be one of these values:
  *            EP_TYPE_CTRL: Control type/
  *            EP_TYPE_ISOC: Isochronous type/
  *            EP_TYPE_BULK: Bulk type/
  *            EP_TYPE_INTR: Interrupt type/
  * @param  token Endpoint Type.
  *          This parameter can be one of these values:
  *            0: HC_PID_SETUP / 1: HC_PID_DATA1
  * @param  pbuff pointer to URB data
  * @param  length Length of URB data
  * @param  do_ping activate do ping protocol (for high speed only).
  *          This parameter can be one of these values:
  *           0 : do ping inactive / 1 : do ping active
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_HC_SubmitRequest(HCD_HandleTypeDef *hhcd,
                                           uint8_t ch_num,
                                           uint8_t direction,
                                           uint8_t ep_type,
                                           uint8_t token,
                                           uint8_t *pbuff,
                                           uint16_t length,
                                           uint8_t do_ping)
{
  hhcd->hc[ch_num].ep_is_in = direction;
  hhcd->hc[ch_num].ep_type  = ep_type;

  if (token == 0U)
  {
    hhcd->hc[ch_num].data_pid = HC_PID_SETUP;
    hhcd->hc[ch_num].do_ping = do_ping;
  }
  else
  {
    hhcd->hc[ch_num].data_pid = HC_PID_DATA1;
  }

  /* Manage Data Toggle */
  switch (ep_type)
  {
    case EP_TYPE_CTRL:
      if (token == 1U) /* send data */
      {
        if (direction == 0U)
        {
          if (length == 0U)
          {
            /* For Status OUT stage, Length == 0U, Status Out PID = 1 */
            hhcd->hc[ch_num].toggle_out = 1U;
          }

          /* Set the Data Toggle bit as per the Flag */
          if (hhcd->hc[ch_num].toggle_out == 0U)
          {
            /* Put the PID 0 */
            hhcd->hc[ch_num].data_pid = HC_PID_DATA0;
          }
          else
          {
            /* Put the PID 1 */
            hhcd->hc[ch_num].data_pid = HC_PID_DATA1;
          }
        }
        else
        {
          if (hhcd->hc[ch_num].do_ssplit == 1U)
          {
            if (hhcd->hc[ch_num].toggle_in == 0U)
            {
              hhcd->hc[ch_num].data_pid = HC_PID_DATA0;
            }
            else
            {
              hhcd->hc[ch_num].data_pid = HC_PID_DATA1;
            }
          }
        }
      }
      break;

    case EP_TYPE_BULK:
      if (direction == 0U)
      {
        /* Set the Data Toggle bit as per the Flag */
        if (hhcd->hc[ch_num].toggle_out == 0U)
        {
          /* Put the PID 0 */
          hhcd->hc[ch_num].data_pid = HC_PID_DATA0;
        }
        else
        {
          /* Put the PID 1 */
          hhcd->hc[ch_num].data_pid = HC_PID_DATA1;
        }
      }
      else
      {
        if (hhcd->hc[ch_num].toggle_in == 0U)
        {
          hhcd->hc[ch_num].data_pid = HC_PID_DATA0;
        }
        else
        {
          hhcd->hc[ch_num].data_pid = HC_PID_DATA1;
        }
      }

      break;
    case EP_TYPE_INTR:
      if (direction == 0U)
      {
        /* Set the Data Toggle bit as per the Flag */
        if (hhcd->hc[ch_num].toggle_out == 0U)
        {
          /* Put the PID 0 */
          hhcd->hc[ch_num].data_pid = HC_PID_DATA0;
        }
        else
        {
          /* Put the PID 1 */
          hhcd->hc[ch_num].data_pid = HC_PID_DATA1;
        }
      }
      else
      {
        if (hhcd->hc[ch_num].toggle_in == 0U)
        {
          hhcd->hc[ch_num].data_pid = HC_PID_DATA0;
        }
        else
        {
          hhcd->hc[ch_num].data_pid = HC_PID_DATA1;
        }
      }
      break;

    case EP_TYPE_ISOC:
      hhcd->hc[ch_num].data_pid = HC_PID_DATA0;
      break;

    default:
      break;
  }

  if (ch_num == 0U)
  {
    s_force_ch0_dispatch_log = 1U;
  }

  hhcd->hc[ch_num].xfer_buff = pbuff;
  hhcd->hc[ch_num].xfer_len  = length;
  hhcd->hc[ch_num].urb_state = URB_IDLE;
  hhcd->hc[ch_num].xfer_count = 0U;
  hhcd->hc[ch_num].ch_num = ch_num;
  hhcd->hc[ch_num].state = HC_IDLE;

  {
    const USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
    uint32_t USBx_BASE = (uint32_t)USBx;

    printf("[HCDSUBMIT] ch=%u dir=%s ep=%u type=%s token=%u len=%u pid=%u mps=%u HCCHAR=0x%08lX HCTSIZ=0x%08lX HCSPLT=0x%08lX HCINT=0x%08lX MSK=0x%08lX\n",
           (unsigned int)ch_num,
           direction ? "IN" : "OUT",
           (unsigned int)hhcd->hc[ch_num].ep_num,
           HCD_EPTypeStr((uint32_t)ep_type),
           (unsigned int)token,
           (unsigned int)length,
           (unsigned int)hhcd->hc[ch_num].data_pid,
           (unsigned int)hhcd->hc[ch_num].max_packet,
           (unsigned long)USBx_HC(ch_num)->HCCHAR,
           (unsigned long)USBx_HC(ch_num)->HCTSIZ,
           (unsigned long)USBx_HC(ch_num)->HCSPLT,
           (unsigned long)USBx_HC(ch_num)->HCINT,
           (unsigned long)USBx_HC(ch_num)->HCINTMSK);

    HAL_StatusTypeDef status = USB_HC_StartXfer(hhcd->Instance, &hhcd->hc[ch_num], (uint8_t)hhcd->Init.dma_enable);

    uint32_t start_hcchar = USBx_HC(ch_num)->HCCHAR;
    uint32_t start_hctsiz = USBx_HC(ch_num)->HCTSIZ;
    uint32_t start_hcsplt = USBx_HC(ch_num)->HCSPLT;
    uint32_t start_hcint = USBx_HC(ch_num)->HCINT;
    uint32_t start_hcintmsk = USBx_HC(ch_num)->HCINTMSK;

    printf("[HCDSTART] ch=%u status=%d HCCHAR=0x%08lX HCTSIZ=0x%08lX HCSPLT=0x%08lX HCINT=0x%08lX MSK=0x%08lX\n",
           (unsigned int)ch_num,
           (int)status,
           (unsigned long)start_hcchar,
           (unsigned long)start_hctsiz,
           (unsigned long)start_hcsplt,
           (unsigned long)start_hcint,
           (unsigned long)start_hcintmsk);

    return status;
  }
}

/**
  * @brief  Handle HCD interrupt request.
  * @param  hhcd HCD handle
  * @retval None
  */
void HAL_HCD_IRQHandler(HCD_HandleTypeDef *hhcd)
{
  USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
  uint32_t USBx_BASE = (uint32_t)USBx;
  uint32_t i;
  uint32_t interrupt;

  HCD_LogGINT(hhcd);

  /* Ensure that we are in device mode */
  if (USB_GetMode(hhcd->Instance) == USB_OTG_MODE_HOST)
  {
    /* Avoid spurious interrupt */
    if (__HAL_HCD_IS_INVALID_INTERRUPT(hhcd))
    {
      return;
    }

    if (__HAL_HCD_GET_FLAG(hhcd, USB_OTG_GINTSTS_PXFR_INCOMPISOOUT))
    {
      /* Incorrect mode, acknowledge the interrupt */
      __HAL_HCD_CLEAR_FLAG(hhcd, USB_OTG_GINTSTS_PXFR_INCOMPISOOUT);
    }

    if (__HAL_HCD_GET_FLAG(hhcd, USB_OTG_GINTSTS_IISOIXFR))
    {
      /* Incorrect mode, acknowledge the interrupt */
      __HAL_HCD_CLEAR_FLAG(hhcd, USB_OTG_GINTSTS_IISOIXFR);
    }

    if (__HAL_HCD_GET_FLAG(hhcd, USB_OTG_GINTSTS_PTXFE))
    {
      /* Incorrect mode, acknowledge the interrupt */
      __HAL_HCD_CLEAR_FLAG(hhcd, USB_OTG_GINTSTS_PTXFE);
    }

    if (__HAL_HCD_GET_FLAG(hhcd, USB_OTG_GINTSTS_MMIS))
    {
      /* Incorrect mode, acknowledge the interrupt */
      __HAL_HCD_CLEAR_FLAG(hhcd, USB_OTG_GINTSTS_MMIS);
    }

    /* Handle Host Disconnect Interrupts */
    if (__HAL_HCD_GET_FLAG(hhcd, USB_OTG_GINTSTS_DISCINT))
    {
      __HAL_HCD_CLEAR_FLAG(hhcd, USB_OTG_GINTSTS_DISCINT);

      if ((USBx_HPRT0 & USB_OTG_HPRT_PCSTS) == 0U)
      {
        /* Flush USB Fifo */
        (void)USB_FlushTxFifo(USBx, 0x10U);
        (void)USB_FlushRxFifo(USBx);

        if (hhcd->Init.phy_itface == USB_OTG_EMBEDDED_PHY)
        {
          /* Restore FS Clock */
          (void)USB_InitFSLSPClkSel(hhcd->Instance, HCFG_48_MHZ);
        }

        /* Handle Host Port Disconnect Interrupt */
#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
        hhcd->DisconnectCallback(hhcd);
#else
        HAL_HCD_Disconnect_Callback(hhcd);
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */
      }
    }

    /* Handle Host Port Interrupts */
    if (__HAL_HCD_GET_FLAG(hhcd, USB_OTG_GINTSTS_HPRTINT))
    {
      HCD_Port_IRQHandler(hhcd);
    }

    /* Handle Host SOF Interrupt */
    if (__HAL_HCD_GET_FLAG(hhcd, USB_OTG_GINTSTS_SOF))
    {
#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
      hhcd->SOFCallback(hhcd);
#else
      HAL_HCD_SOF_Callback(hhcd);
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */

      __HAL_HCD_CLEAR_FLAG(hhcd, USB_OTG_GINTSTS_SOF);
    }

    /* Handle Host channel Interrupt */
    if (__HAL_HCD_GET_FLAG(hhcd, USB_OTG_GINTSTS_HCINT))
    {
      interrupt = USB_HC_ReadInterrupt(hhcd->Instance);
      HCD_LogHAINT(interrupt);
      if (interrupt == 0U)
      {
        HCD_LogDispatch(hhcd, 0U);
      }
      for (i = 0U; i < hhcd->Init.Host_channels; i++)
      {
        if ((interrupt & (1UL << (i & 0xFU))) != 0U)
        {
          HCD_LogDispatch(hhcd, (uint8_t)i);
          HCD_LogIRQ(hhcd, (uint8_t)i);

          if ((USBx_HC(i)->HCCHAR & USB_OTG_HCCHAR_EPDIR) == USB_OTG_HCCHAR_EPDIR)
          {
            HCD_HC_IN_IRQHandler(hhcd, (uint8_t)i);
          }
          else
          {
            HCD_HC_OUT_IRQHandler(hhcd, (uint8_t)i);
          }
        }
      }
      __HAL_HCD_CLEAR_FLAG(hhcd, USB_OTG_GINTSTS_HCINT);
    }

    /* Handle Rx Queue Level Interrupts */
    if ((__HAL_HCD_GET_FLAG(hhcd, USB_OTG_GINTSTS_RXFLVL)) != 0U)
    {
      USB_MASK_INTERRUPT(hhcd->Instance, USB_OTG_GINTSTS_RXFLVL);

      HCD_RXQLVL_IRQHandler(hhcd);

      USB_UNMASK_INTERRUPT(hhcd->Instance, USB_OTG_GINTSTS_RXFLVL);
    }
  }
}


/**
  * @brief  SOF callback.
  * @param  hhcd HCD handle
  * @retval None
  */
__weak void HAL_HCD_SOF_Callback(HCD_HandleTypeDef *hhcd)
{
  /* Prevent unused argument(s) compilation warning */
  UNUSED(hhcd);

  /* NOTE : This function should not be modified, when the callback is needed,
            the HAL_HCD_SOF_Callback could be implemented in the user file
   */
}

/**
  * @brief Connection Event callback.
  * @param  hhcd HCD handle
  * @retval None
  */
__weak void HAL_HCD_Connect_Callback(HCD_HandleTypeDef *hhcd)
{
  /* Prevent unused argument(s) compilation warning */
  UNUSED(hhcd);

  /* NOTE : This function should not be modified, when the callback is needed,
            the HAL_HCD_Connect_Callback could be implemented in the user file
   */
}

/**
  * @brief  Disconnection Event callback.
  * @param  hhcd HCD handle
  * @retval None
  */
__weak void HAL_HCD_Disconnect_Callback(HCD_HandleTypeDef *hhcd)
{
  /* Prevent unused argument(s) compilation warning */
  UNUSED(hhcd);

  /* NOTE : This function should not be modified, when the callback is needed,
            the HAL_HCD_Disconnect_Callback could be implemented in the user file
   */
}

/**
  * @brief  Port Enabled  Event callback.
  * @param  hhcd HCD handle
  * @retval None
  */
__weak void HAL_HCD_PortEnabled_Callback(HCD_HandleTypeDef *hhcd)
{
  /* Prevent unused argument(s) compilation warning */
  UNUSED(hhcd);

  /* NOTE : This function should not be modified, when the callback is needed,
            the HAL_HCD_Disconnect_Callback could be implemented in the user file
   */
}

/**
  * @brief  Port Disabled  Event callback.
  * @param  hhcd HCD handle
  * @retval None
  */
__weak void HAL_HCD_PortDisabled_Callback(HCD_HandleTypeDef *hhcd)
{
  /* Prevent unused argument(s) compilation warning */
  UNUSED(hhcd);

  /* NOTE : This function should not be modified, when the callback is needed,
            the HAL_HCD_Disconnect_Callback could be implemented in the user file
   */
}

/**
  * @brief  Notify URB state change callback.
  * @param  hhcd HCD handle
  * @param  chnum Channel number.
  *         This parameter can be a value from 1 to 15
  * @param  urb_state:
  *          This parameter can be one of these values:
  *            URB_IDLE/
  *            URB_DONE/
  *            URB_NOTREADY/
  *            URB_NYET/
  *            URB_ERROR/
  *            URB_STALL/
  * @retval None
  */
__weak void HAL_HCD_HC_NotifyURBChange_Callback(HCD_HandleTypeDef *hhcd, uint8_t chnum, HCD_URBStateTypeDef urb_state)
{
  /* Prevent unused argument(s) compilation warning */
  UNUSED(hhcd);
  UNUSED(chnum);
  UNUSED(urb_state);

  /* NOTE : This function should not be modified, when the callback is needed,
            the HAL_HCD_HC_NotifyURBChange_Callback could be implemented in the user file
   */
}

#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
/**
  * @brief  Register a User USB HCD Callback
  *         To be used instead of the weak predefined callback
  * @param  hhcd USB HCD handle
  * @param  CallbackID ID of the callback to be registered
  *         This parameter can be one of the following values:
  *          @arg @ref HAL_HCD_SOF_CB_ID USB HCD SOF callback ID
  *          @arg @ref HAL_HCD_CONNECT_CB_ID USB HCD Connect callback ID
  *          @arg @ref HAL_HCD_DISCONNECT_CB_ID OTG HCD Disconnect callback ID
  *          @arg @ref HAL_HCD_PORT_ENABLED_CB_ID USB HCD Port Enable callback ID
  *          @arg @ref HAL_HCD_PORT_DISABLED_CB_ID USB HCD Port Disable callback ID
  *          @arg @ref HAL_HCD_MSPINIT_CB_ID MspDeInit callback ID
  *          @arg @ref HAL_HCD_MSPDEINIT_CB_ID MspDeInit callback ID
  * @param  pCallback pointer to the Callback function
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_RegisterCallback(HCD_HandleTypeDef *hhcd,
                                           HAL_HCD_CallbackIDTypeDef CallbackID,
                                           pHCD_CallbackTypeDef pCallback)
{
  HAL_StatusTypeDef status = HAL_OK;

  if (pCallback == NULL)
  {
    /* Update the error code */
    hhcd->ErrorCode |= HAL_HCD_ERROR_INVALID_CALLBACK;
    return HAL_ERROR;
  }
  /* Process locked */
  __HAL_LOCK(hhcd);

  if (hhcd->State == HAL_HCD_STATE_READY)
  {
    switch (CallbackID)
    {
      case HAL_HCD_SOF_CB_ID :
        hhcd->SOFCallback = pCallback;
        break;

      case HAL_HCD_CONNECT_CB_ID :
        hhcd->ConnectCallback = pCallback;
        break;

      case HAL_HCD_DISCONNECT_CB_ID :
        hhcd->DisconnectCallback = pCallback;
        break;

      case HAL_HCD_PORT_ENABLED_CB_ID :
        hhcd->PortEnabledCallback = pCallback;
        break;

      case HAL_HCD_PORT_DISABLED_CB_ID :
        hhcd->PortDisabledCallback = pCallback;
        break;

      case HAL_HCD_MSPINIT_CB_ID :
        hhcd->MspInitCallback = pCallback;
        break;

      case HAL_HCD_MSPDEINIT_CB_ID :
        hhcd->MspDeInitCallback = pCallback;
        break;

      default :
        /* Update the error code */
        hhcd->ErrorCode |= HAL_HCD_ERROR_INVALID_CALLBACK;
        /* Return error status */
        status =  HAL_ERROR;
        break;
    }
  }
  else if (hhcd->State == HAL_HCD_STATE_RESET)
  {
    switch (CallbackID)
    {
      case HAL_HCD_MSPINIT_CB_ID :
        hhcd->MspInitCallback = pCallback;
        break;

      case HAL_HCD_MSPDEINIT_CB_ID :
        hhcd->MspDeInitCallback = pCallback;
        break;

      default :
        /* Update the error code */
        hhcd->ErrorCode |= HAL_HCD_ERROR_INVALID_CALLBACK;
        /* Return error status */
        status =  HAL_ERROR;
        break;
    }
  }
  else
  {
    /* Update the error code */
    hhcd->ErrorCode |= HAL_HCD_ERROR_INVALID_CALLBACK;
    /* Return error status */
    status =  HAL_ERROR;
  }

  /* Release Lock */
  __HAL_UNLOCK(hhcd);
  return status;
}

/**
  * @brief  Unregister an USB HCD Callback
  *         USB HCD callback is redirected to the weak predefined callback
  * @param  hhcd USB HCD handle
  * @param  CallbackID ID of the callback to be unregistered
  *         This parameter can be one of the following values:
  *          @arg @ref HAL_HCD_SOF_CB_ID USB HCD SOF callback ID
  *          @arg @ref HAL_HCD_CONNECT_CB_ID USB HCD Connect callback ID
  *          @arg @ref HAL_HCD_DISCONNECT_CB_ID OTG HCD Disconnect callback ID
  *          @arg @ref HAL_HCD_PORT_ENABLED_CB_ID USB HCD Port Enabled callback ID
  *          @arg @ref HAL_HCD_PORT_DISABLED_CB_ID USB HCD Port Disabled callback ID
  *          @arg @ref HAL_HCD_MSPINIT_CB_ID MspDeInit callback ID
  *          @arg @ref HAL_HCD_MSPDEINIT_CB_ID MspDeInit callback ID
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_UnRegisterCallback(HCD_HandleTypeDef *hhcd, HAL_HCD_CallbackIDTypeDef CallbackID)
{
  HAL_StatusTypeDef status = HAL_OK;

  /* Process locked */
  __HAL_LOCK(hhcd);

  /* Setup Legacy weak Callbacks  */
  if (hhcd->State == HAL_HCD_STATE_READY)
  {
    switch (CallbackID)
    {
      case HAL_HCD_SOF_CB_ID :
        hhcd->SOFCallback = HAL_HCD_SOF_Callback;
        break;

      case HAL_HCD_CONNECT_CB_ID :
        hhcd->ConnectCallback = HAL_HCD_Connect_Callback;
        break;

      case HAL_HCD_DISCONNECT_CB_ID :
        hhcd->DisconnectCallback = HAL_HCD_Disconnect_Callback;
        break;

      case HAL_HCD_PORT_ENABLED_CB_ID :
        hhcd->PortEnabledCallback = HAL_HCD_PortEnabled_Callback;
        break;

      case HAL_HCD_PORT_DISABLED_CB_ID :
        hhcd->PortDisabledCallback = HAL_HCD_PortDisabled_Callback;
        break;

      case HAL_HCD_MSPINIT_CB_ID :
        hhcd->MspInitCallback = HAL_HCD_MspInit;
        break;

      case HAL_HCD_MSPDEINIT_CB_ID :
        hhcd->MspDeInitCallback = HAL_HCD_MspDeInit;
        break;

      default :
        /* Update the error code */
        hhcd->ErrorCode |= HAL_HCD_ERROR_INVALID_CALLBACK;

        /* Return error status */
        status =  HAL_ERROR;
        break;
    }
  }
  else if (hhcd->State == HAL_HCD_STATE_RESET)
  {
    switch (CallbackID)
    {
      case HAL_HCD_MSPINIT_CB_ID :
        hhcd->MspInitCallback = HAL_HCD_MspInit;
        break;

      case HAL_HCD_MSPDEINIT_CB_ID :
        hhcd->MspDeInitCallback = HAL_HCD_MspDeInit;
        break;

      default :
        /* Update the error code */
        hhcd->ErrorCode |= HAL_HCD_ERROR_INVALID_CALLBACK;

        /* Return error status */
        status =  HAL_ERROR;
        break;
    }
  }
  else
  {
    /* Update the error code */
    hhcd->ErrorCode |= HAL_HCD_ERROR_INVALID_CALLBACK;

    /* Return error status */
    status =  HAL_ERROR;
  }

  /* Release Lock */
  __HAL_UNLOCK(hhcd);
  return status;
}

/**
  * @brief  Register USB HCD Host Channel Notify URB Change Callback
  *         To be used instead of the weak HAL_HCD_HC_NotifyURBChange_Callback() predefined callback
  * @param  hhcd HCD handle
  * @param  pCallback pointer to the USB HCD Host Channel Notify URB Change Callback function
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_RegisterHC_NotifyURBChangeCallback(HCD_HandleTypeDef *hhcd,
                                                             pHCD_HC_NotifyURBChangeCallbackTypeDef pCallback)
{
  HAL_StatusTypeDef status = HAL_OK;

  if (pCallback == NULL)
  {
    /* Update the error code */
    hhcd->ErrorCode |= HAL_HCD_ERROR_INVALID_CALLBACK;

    return HAL_ERROR;
  }

  /* Process locked */
  __HAL_LOCK(hhcd);

  if (hhcd->State == HAL_HCD_STATE_READY)
  {
    hhcd->HC_NotifyURBChangeCallback = pCallback;
  }
  else
  {
    /* Update the error code */
    hhcd->ErrorCode |= HAL_HCD_ERROR_INVALID_CALLBACK;

    /* Return error status */
    status =  HAL_ERROR;
  }

  /* Release Lock */
  __HAL_UNLOCK(hhcd);

  return status;
}

/**
  * @brief  Unregister the USB HCD Host Channel Notify URB Change Callback
  *         USB HCD Host Channel Notify URB Change Callback is redirected
  *         to the weak HAL_HCD_HC_NotifyURBChange_Callback() predefined callback
  * @param  hhcd HCD handle
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_UnRegisterHC_NotifyURBChangeCallback(HCD_HandleTypeDef *hhcd)
{
  HAL_StatusTypeDef status = HAL_OK;

  /* Process locked */
  __HAL_LOCK(hhcd);

  if (hhcd->State == HAL_HCD_STATE_READY)
  {
    hhcd->HC_NotifyURBChangeCallback = HAL_HCD_HC_NotifyURBChange_Callback; /* Legacy weak DataOutStageCallback  */
  }
  else
  {
    /* Update the error code */
    hhcd->ErrorCode |= HAL_HCD_ERROR_INVALID_CALLBACK;

    /* Return error status */
    status =  HAL_ERROR;
  }

  /* Release Lock */
  __HAL_UNLOCK(hhcd);

  return status;
}
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */

/**
  * @}
  */

/** @defgroup HCD_Exported_Functions_Group3 Peripheral Control functions
  *  @brief   Management functions
  *
@verbatim
 ===============================================================================
                      ##### Peripheral Control functions #####
 ===============================================================================
    [..]
    This subsection provides a set of functions allowing to control the HCD data
    transfers.

@endverbatim
  * @{
  */

/**
  * @brief  Start the host driver.
  * @param  hhcd HCD handle
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_Start(HCD_HandleTypeDef *hhcd)
{
  __HAL_LOCK(hhcd);
  /* Enable port power */
  (void)USB_DriveVbus(hhcd->Instance, 1U);

  /* Enable global interrupt */
  __HAL_HCD_ENABLE(hhcd);
  __HAL_UNLOCK(hhcd);

  return HAL_OK;
}

/**
  * @brief  Stop the host driver.
  * @param  hhcd HCD handle
  * @retval HAL status
  */

HAL_StatusTypeDef HAL_HCD_Stop(HCD_HandleTypeDef *hhcd)
{
  __HAL_LOCK(hhcd);
  (void)USB_StopHost(hhcd->Instance);
  __HAL_UNLOCK(hhcd);

  return HAL_OK;
}

/**
  * @brief  Reset the host port.
  * @param  hhcd HCD handle
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_ResetPort(HCD_HandleTypeDef *hhcd)
{
  return (USB_ResetPort(hhcd->Instance));
}

/**
  * @}
  */

/** @defgroup HCD_Exported_Functions_Group4 Peripheral State functions
  *  @brief   Peripheral State functions
  *
@verbatim
 ===============================================================================
                      ##### Peripheral State functions #####
 ===============================================================================
    [..]
    This subsection permits to get in run-time the status of the peripheral
    and the data flow.

@endverbatim
  * @{
  */

/**
  * @brief  Return the HCD handle state.
  * @param  hhcd HCD handle
  * @retval HAL state
  */
HCD_StateTypeDef HAL_HCD_GetState(HCD_HandleTypeDef const *hhcd)
{
  return hhcd->State;
}

/**
  * @brief  Return  URB state for a channel.
  * @param  hhcd HCD handle
  * @param  chnum Channel number.
  *         This parameter can be a value from 1 to 15
  * @retval URB state.
  *          This parameter can be one of these values:
  *            URB_IDLE/
  *            URB_DONE/
  *            URB_NOTREADY/
  *            URB_NYET/
  *            URB_ERROR/
  *            URB_STALL
  */
HCD_URBStateTypeDef HAL_HCD_HC_GetURBState(HCD_HandleTypeDef const *hhcd, uint8_t chnum)
{
  return hhcd->hc[chnum].urb_state;
}


/**
  * @brief  Return the last host transfer size.
  * @param  hhcd HCD handle
  * @param  chnum Channel number.
  *         This parameter can be a value from 1 to 15
  * @retval last transfer size in byte
  */
uint32_t HAL_HCD_HC_GetXferCount(HCD_HandleTypeDef const *hhcd, uint8_t chnum)
{
  return hhcd->hc[chnum].xfer_count;
}

/**
  * @brief  Return the Host Channel state.
  * @param  hhcd HCD handle
  * @param  chnum Channel number.
  *         This parameter can be a value from 1 to 15
  * @retval Host channel state
  *          This parameter can be one of these values:
  *            HC_IDLE/
  *            HC_XFRC/
  *            HC_HALTED/
  *            HC_NYET/
  *            HC_NAK/
  *            HC_STALL/
  *            HC_XACTERR/
  *            HC_BBLERR/
  *            HC_DATATGLERR
  */
HCD_HCStateTypeDef  HAL_HCD_HC_GetState(HCD_HandleTypeDef const *hhcd, uint8_t chnum)
{
  return hhcd->hc[chnum].state;
}

/**
  * @brief  Return the current Host frame number.
  * @param  hhcd HCD handle
  * @retval Current Host frame number
  */
uint32_t HAL_HCD_GetCurrentFrame(HCD_HandleTypeDef *hhcd)
{
  return (USB_GetCurrentFrame(hhcd->Instance));
}

/**
  * @brief  Return the Host enumeration speed.
  * @param  hhcd HCD handle
  * @retval Enumeration speed
  */
uint32_t HAL_HCD_GetCurrentSpeed(HCD_HandleTypeDef *hhcd)
{
  return (USB_GetHostSpeed(hhcd->Instance));
}

/**
  * @brief  Set host channel Hub information.
  * @param  hhcd HCD handle
  * @param  ch_num Channel number.
  *         This parameter can be a value from 1 to 15
  * @param  addr Hub address
  * @param  PortNbr Hub port number
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_HC_SetHubInfo(HCD_HandleTypeDef *hhcd, uint8_t ch_num,
                                        uint8_t addr, uint8_t PortNbr)
{
  uint32_t HostCoreSpeed = USB_GetHostSpeed(hhcd->Instance);

  /* LS/FS device plugged to HS HUB */
  if ((hhcd->hc[ch_num].speed != HCD_DEVICE_SPEED_HIGH) && (HostCoreSpeed == HPRT0_PRTSPD_HIGH_SPEED))
  {
    hhcd->hc[ch_num].do_ssplit = 1U;

    if ((hhcd->hc[ch_num].ep_type == EP_TYPE_CTRL) && (hhcd->hc[ch_num].ep_is_in != 0U))
    {
      hhcd->hc[ch_num].toggle_in = 1U;
    }
  }

  hhcd->hc[ch_num].hub_addr = addr;
  hhcd->hc[ch_num].hub_port_nbr = PortNbr;

  return HAL_OK;
}


/**
  * @brief  Clear host channel hub information.
  * @param  hhcd HCD handle
  * @param  ch_num Channel number.
  *         This parameter can be a value from 1 to 15
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_HC_ClearHubInfo(HCD_HandleTypeDef *hhcd, uint8_t ch_num)
{
  hhcd->hc[ch_num].do_ssplit = 0U;
  hhcd->hc[ch_num].do_csplit = 0U;
  hhcd->hc[ch_num].hub_addr = 0U;
  hhcd->hc[ch_num].hub_port_nbr = 0U;

  return HAL_OK;
}


/** @brief  Activate a host channel.
  * @param  hhcd HCD handle
  * @param  ch_num Channel number.
  *         This parameter can be a value from 1 to 15
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_HCD_HC_Activate(HCD_HandleTypeDef *hhcd, uint8_t ch_num)
{
  HAL_StatusTypeDef status = HAL_OK;

  __HAL_LOCK(hhcd);
  (void)USB_HC_Activate(hhcd->Instance, (uint8_t)ch_num, hhcd->hc[ch_num].ch_dir);
  __HAL_UNLOCK(hhcd);

  return status;
}
/**
  * @}
  */

/**
  * @}
  */

/** @addtogroup HCD_Private_Functions
  * @{
  */
/**
  * @brief  Handle Host Channel IN interrupt requests.
  * @param  hhcd HCD handle
  * @param  chnum Channel number.
  *         This parameter can be a value from 1 to 15
  * @retval none
  */
static void HCD_HC_IN_IRQHandler(HCD_HandleTypeDef *hhcd, uint8_t chnum)
{
  const USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
  uint32_t USBx_BASE = (uint32_t)USBx;
  uint32_t tmpreg;
  uint32_t raw_hcint = USBx_HC(chnum)->HCINT;
  uint32_t raw_hcintmsk = USBx_HC(chnum)->HCINTMSK;
  HCD_HCStateTypeDef prev_state = hhcd->hc[chnum].state;

  if ((raw_hcint & ~USB_OTG_HCINT_CHH) != 0U)
  {
    s_last_cause_hcint[chnum] = (raw_hcint & ~USB_OTG_HCINT_CHH);
  }

  HCD_LogHC(hhcd, chnum, 1U, raw_hcint, raw_hcintmsk);

  if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_AHBERR))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_AHBERR);
    hhcd->hc[chnum].state = HC_XACTERR;
    (void)USB_HC_Halt(hhcd->Instance, chnum);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_BBERR))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_BBERR);
    hhcd->hc[chnum].state = HC_BBLERR;
    (void)USB_HC_Halt(hhcd->Instance, chnum);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_STALL))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_STALL);
    hhcd->hc[chnum].state = HC_STALL;
    (void)USB_HC_Halt(hhcd->Instance, chnum);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_DTERR))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_DTERR);
    hhcd->hc[chnum].state = HC_DATATGLERR;
    (void)USB_HC_Halt(hhcd->Instance, chnum);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_TXERR))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_TXERR);
    hhcd->hc[chnum].state = HC_XACTERR;
    (void)USB_HC_Halt(hhcd->Instance, chnum);
  }
  else
  {
    /* ... */
  }

  if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_FRMOR))
  {
    (void)USB_HC_Halt(hhcd->Instance, chnum);
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_FRMOR);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_XFRC))
  {
    /* Clear any pending ACK IT */
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_ACK);

    if (hhcd->hc[chnum].do_csplit == 1U)
    {
      hhcd->hc[chnum].do_csplit = 0U;
      __HAL_HCD_CLEAR_HC_CSPLT(chnum);
    }

    if (hhcd->Init.dma_enable != 0U)
    {
      hhcd->hc[chnum].xfer_count = hhcd->hc[chnum].XferSize - (USBx_HC(chnum)->HCTSIZ & USB_OTG_HCTSIZ_XFRSIZ);
    }

    hhcd->hc[chnum].state = HC_XFRC;
    hhcd->hc[chnum].ErrCnt = 0U;
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_XFRC);

    if ((hhcd->hc[chnum].ep_type == EP_TYPE_CTRL) ||
        (hhcd->hc[chnum].ep_type == EP_TYPE_BULK))
    {
      (void)USB_HC_Halt(hhcd->Instance, chnum);
      __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_NAK);
    }
    else if ((hhcd->hc[chnum].ep_type == EP_TYPE_INTR) ||
             (hhcd->hc[chnum].ep_type == EP_TYPE_ISOC))
    {
      USBx_HC(chnum)->HCCHAR |= USB_OTG_HCCHAR_ODDFRM;
      hhcd->hc[chnum].urb_state = URB_DONE;

#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
      hhcd->HC_NotifyURBChangeCallback(hhcd, chnum, hhcd->hc[chnum].urb_state);
#else
      HAL_HCD_HC_NotifyURBChange_Callback(hhcd, chnum, hhcd->hc[chnum].urb_state);
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */
    }
    else
    {
      /* ... */
    }

    if (hhcd->Init.dma_enable == 1U)
    {
      if ((((hhcd->hc[chnum].xfer_count + hhcd->hc[chnum].max_packet - 1U) / hhcd->hc[chnum].max_packet) & 1U) != 0U)
      {
        hhcd->hc[chnum].toggle_in ^= 1U;
      }
    }
    else
    {
      hhcd->hc[chnum].toggle_in ^= 1U;
    }
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_ACK))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_ACK);

    hhcd->hc[chnum].NakCnt = 0U;

    if (hhcd->hc[chnum].do_ssplit == 1U)
    {
      hhcd->hc[chnum].do_csplit = 1U;
      hhcd->hc[chnum].state = HC_ACK;

      (void)USB_HC_Halt(hhcd->Instance, chnum);
    }
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_CHH))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_CHH);

    tmpreg = USBx_HC(chnum)->HCCHAR;

    if ((tmpreg & USB_OTG_HCCHAR_CHDIS) != 0U)
    {
      /* Halt received while channel disable still in progress */
      return;
    }

    if (hhcd->hc[chnum].state == HC_XFRC)
    {
      hhcd->hc[chnum].state = HC_HALTED;
      hhcd->hc[chnum].urb_state = URB_DONE;
    }
    else if (hhcd->hc[chnum].state == HC_STALL)
    {
      hhcd->hc[chnum].state = HC_HALTED;
      hhcd->hc[chnum].urb_state = URB_STALL;
    }
    else if ((hhcd->hc[chnum].state == HC_XACTERR) ||
             (hhcd->hc[chnum].state == HC_DATATGLERR))
    {
      hhcd->hc[chnum].state = HC_HALTED;

      if (hhcd->Init.dma_enable == 0U)
      {
        hhcd->hc[chnum].ErrCnt++;

        if (hhcd->hc[chnum].ErrCnt > 2U)
        {
          hhcd->hc[chnum].ErrCnt = 0U;

          if (hhcd->hc[chnum].do_ssplit == 1U)
          {
            hhcd->hc[chnum].do_csplit = 0U;
            hhcd->hc[chnum].ep_ss_schedule = 0U;
            __HAL_HCD_CLEAR_HC_CSPLT(chnum);
          }

          hhcd->hc[chnum].urb_state = URB_ERROR;
        }
        else
        {
          hhcd->hc[chnum].urb_state = URB_NOTREADY;

          if ((hhcd->hc[chnum].ep_type == EP_TYPE_CTRL) ||
              (hhcd->hc[chnum].ep_type == EP_TYPE_BULK))
          {
            /* re-activate the channel */
            tmpreg = USBx_HC(chnum)->HCCHAR;
            tmpreg &= ~USB_OTG_HCCHAR_CHDIS;
            tmpreg |= USB_OTG_HCCHAR_CHENA;
            USBx_HC(chnum)->HCCHAR = tmpreg;
          }
        }
      }
      else
      {
        hhcd->hc[chnum].urb_state = URB_NOTREADY;

        if ((hhcd->hc[chnum].ep_type == EP_TYPE_CTRL) ||
            (hhcd->hc[chnum].ep_type == EP_TYPE_BULK))
        {
          /* re-activate the channel */
          tmpreg = USBx_HC(chnum)->HCCHAR;
          tmpreg &= ~USB_OTG_HCCHAR_CHDIS;
          tmpreg |= USB_OTG_HCCHAR_CHENA;
          USBx_HC(chnum)->HCCHAR = tmpreg;
        }
      }
    }
    else if (hhcd->hc[chnum].state == HC_NYET)
    {
      hhcd->hc[chnum].state = HC_HALTED;

      if (hhcd->hc[chnum].do_csplit == 1U)
      {
        if (hhcd->hc[chnum].ep_type == EP_TYPE_INTR)
        {
          hhcd->hc[chnum].NyetErrCnt++;
          if (hhcd->hc[chnum].NyetErrCnt > 2U)
          {
            hhcd->hc[chnum].NyetErrCnt = 0U;
            hhcd->hc[chnum].do_csplit = 0U;

            if (hhcd->hc[chnum].ErrCnt < 3U)
            {
              hhcd->hc[chnum].ep_ss_schedule = 1U;
            }
            __HAL_HCD_CLEAR_HC_CSPLT(chnum);
            hhcd->hc[chnum].urb_state = URB_ERROR;
          }
          else
          {
            hhcd->hc[chnum].urb_state = URB_NOTREADY;
          }
        }
        else
        {
          hhcd->hc[chnum].urb_state = URB_NOTREADY;
        }

        if ((hhcd->hc[chnum].ep_type == EP_TYPE_CTRL) ||
            (hhcd->hc[chnum].ep_type == EP_TYPE_BULK))
        {
          /* re-activate the channel */
          tmpreg = USBx_HC(chnum)->HCCHAR;
          tmpreg &= ~USB_OTG_HCCHAR_CHDIS;
          tmpreg |= USB_OTG_HCCHAR_CHENA;
          USBx_HC(chnum)->HCCHAR = tmpreg;
        }
      }
    }
    else if (hhcd->hc[chnum].state == HC_ACK)
    {
      hhcd->hc[chnum].state = HC_HALTED;

      if (hhcd->hc[chnum].do_csplit == 1U)
      {
        hhcd->hc[chnum].urb_state = URB_NOTREADY;

        /* Set Complete split and re-activate the channel */
        USBx_HC(chnum)->HCSPLT |= USB_OTG_HCSPLT_COMPLSPLT;
        USBx_HC(chnum)->HCINTMSK |= USB_OTG_HCINTMSK_NYET;
        USBx_HC(chnum)->HCINTMSK &= ~USB_OTG_HCINT_ACK;

        if ((hhcd->hc[chnum].ep_type == EP_TYPE_CTRL) ||
            (hhcd->hc[chnum].ep_type == EP_TYPE_BULK))
        {
          /* re-activate the channel */
          tmpreg = USBx_HC(chnum)->HCCHAR;
          tmpreg &= ~USB_OTG_HCCHAR_CHDIS;
          tmpreg |= USB_OTG_HCCHAR_CHENA;
          USBx_HC(chnum)->HCCHAR = tmpreg;
        }
      }
    }
    else if (hhcd->hc[chnum].state == HC_NAK)
    {
      hhcd->hc[chnum].state = HC_HALTED;
      hhcd->hc[chnum].urb_state = URB_NOTREADY;

      if ((hhcd->hc[chnum].ep_type == EP_TYPE_CTRL) ||
          (hhcd->hc[chnum].ep_type == EP_TYPE_BULK))
      {
#if defined (USE_HAL_HCD_IN_NAK_AUTO_ACTIVATE_DISABLE) && (USE_HAL_HCD_IN_NAK_AUTO_ACTIVATE_DISABLE == 1)
        hhcd->hc[chnum].NakCnt++;

        if (hhcd->hc[chnum].NakCnt >= HAL_HCD_CHANNEL_NAK_COUNT)
        {
          hhcd->hc[chnum].state = HC_IDLE;
          hhcd->hc[chnum].urb_state = URB_NAK_WAIT;
          hhcd->hc[chnum].NakCnt = 0U;
        }
        else
#endif /* defined (USE_HAL_HCD_IN_NAK_AUTO_ACTIVATE_DISABLE) && (USE_HAL_HCD_IN_NAK_AUTO_ACTIVATE_DISABLE == 1) */
        {
          /* re-activate the channel */
          tmpreg = USBx_HC(chnum)->HCCHAR;
          tmpreg &= ~USB_OTG_HCCHAR_CHDIS;
          tmpreg |= USB_OTG_HCCHAR_CHENA;
          USBx_HC(chnum)->HCCHAR = tmpreg;
        }
      }
    }
    else if (hhcd->hc[chnum].state == HC_BBLERR)
    {
      hhcd->hc[chnum].state = HC_HALTED;
      hhcd->hc[chnum].ErrCnt++;
      hhcd->hc[chnum].urb_state = URB_ERROR;
    }
    else
    {
      if (hhcd->hc[chnum].state == HC_HALTED)
      {
        return;
      }
    }

    HCD_LogCHH(hhcd, chnum, 1U, (uint32_t)prev_state, raw_hcint, raw_hcintmsk);

    if (hhcd->hc[chnum].urb_state == URB_NOTREADY)
    {
      HCD_LogNotReady(hhcd, chnum, raw_hcint, raw_hcintmsk);
    }

#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
    hhcd->HC_NotifyURBChangeCallback(hhcd, chnum, hhcd->hc[chnum].urb_state);
#else
    HAL_HCD_HC_NotifyURBChange_Callback(hhcd, chnum, hhcd->hc[chnum].urb_state);
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_NYET))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_NYET);
    hhcd->hc[chnum].state = HC_NYET;

    if (hhcd->hc[chnum].do_ssplit == 0U)
    {
      hhcd->hc[chnum].ErrCnt = 0U;
    }

    (void)USB_HC_Halt(hhcd->Instance, chnum);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_NAK))
  {
    if (hhcd->hc[chnum].ep_type == EP_TYPE_INTR)
    {
      hhcd->hc[chnum].ErrCnt = 0U;
      hhcd->hc[chnum].state = HC_NAK;
      (void)USB_HC_Halt(hhcd->Instance, chnum);
    }
    else if ((hhcd->hc[chnum].ep_type == EP_TYPE_CTRL) ||
             (hhcd->hc[chnum].ep_type == EP_TYPE_BULK))
    {
      hhcd->hc[chnum].ErrCnt = 0U;

      if ((hhcd->Init.dma_enable == 0U) || (hhcd->hc[chnum].do_csplit == 1U))
      {
        hhcd->hc[chnum].state = HC_NAK;
        (void)USB_HC_Halt(hhcd->Instance, chnum);
      }
    }
    else
    {
      /* ... */
    }

    if (hhcd->hc[chnum].do_csplit == 1U)
    {
      hhcd->hc[chnum].do_csplit = 0U;
      __HAL_HCD_CLEAR_HC_CSPLT(chnum);
      __HAL_HCD_UNMASK_ACK_HC_INT(chnum);
    }

    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_NAK);
  }
  else
  {
    /* ... */
  }
}

/**
  * @brief  Handle Host Channel OUT interrupt requests.
  * @param  hhcd HCD handle
  * @param  chnum Channel number.
  *         This parameter can be a value from 1 to 15
  * @retval none
  */
static void HCD_HC_OUT_IRQHandler(HCD_HandleTypeDef *hhcd, uint8_t chnum)
{
  const USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
  uint32_t USBx_BASE = (uint32_t)USBx;
  uint32_t tmpreg;
  uint32_t num_packets;
  uint32_t raw_hcint = USBx_HC(chnum)->HCINT;
  uint32_t raw_hcintmsk = USBx_HC(chnum)->HCINTMSK;
  HCD_HCStateTypeDef prev_state = hhcd->hc[chnum].state;

  if ((raw_hcint & ~USB_OTG_HCINT_CHH) != 0U)
  {
    s_last_cause_hcint[chnum] = (raw_hcint & ~USB_OTG_HCINT_CHH);
  }

  HCD_LogHC(hhcd, chnum, 0U, raw_hcint, raw_hcintmsk);

  if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_AHBERR))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_AHBERR);
    hhcd->hc[chnum].state = HC_XACTERR;
    (void)USB_HC_Halt(hhcd->Instance, chnum);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_ACK))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_ACK);

    if (hhcd->hc[chnum].do_ping == 1U)
    {
      hhcd->hc[chnum].do_ping = 0U;
      hhcd->hc[chnum].urb_state = URB_NOTREADY;
      hhcd->hc[chnum].state = HC_ACK;
      HCD_LogNotReady(hhcd, chnum, raw_hcint, raw_hcintmsk);
      (void)USB_HC_Halt(hhcd->Instance, chnum);
    }

    if ((hhcd->hc[chnum].do_ssplit == 1U) && (hhcd->hc[chnum].do_csplit == 0U))
    {
      if (hhcd->hc[chnum].ep_type != EP_TYPE_ISOC)
      {
        hhcd->hc[chnum].do_csplit = 1U;
      }

      hhcd->hc[chnum].state = HC_ACK;
      (void)USB_HC_Halt(hhcd->Instance, chnum);

      /* reset error_count */
      hhcd->hc[chnum].ErrCnt = 0U;
    }
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_FRMOR))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_FRMOR);
    (void)USB_HC_Halt(hhcd->Instance, chnum);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_XFRC))
  {
    hhcd->hc[chnum].ErrCnt = 0U;

    /* transaction completed with NYET state, update do ping state */
    if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_NYET))
    {
      hhcd->hc[chnum].do_ping = 1U;
      __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_NYET);
    }

    if (hhcd->hc[chnum].do_csplit != 0U)
    {
      hhcd->hc[chnum].do_csplit = 0U;
      __HAL_HCD_CLEAR_HC_CSPLT(chnum);
    }

    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_XFRC);
    hhcd->hc[chnum].state = HC_XFRC;
    (void)USB_HC_Halt(hhcd->Instance, chnum);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_NYET))
  {
    hhcd->hc[chnum].state = HC_NYET;

    if (hhcd->hc[chnum].do_ssplit == 0U)
    {
      hhcd->hc[chnum].do_ping = 1U;
    }

    hhcd->hc[chnum].ErrCnt = 0U;
    (void)USB_HC_Halt(hhcd->Instance, chnum);
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_NYET);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_STALL))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_STALL);
    hhcd->hc[chnum].state = HC_STALL;
    (void)USB_HC_Halt(hhcd->Instance, chnum);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_NAK))
  {
    hhcd->hc[chnum].ErrCnt = 0U;
    hhcd->hc[chnum].state = HC_NAK;

    if (hhcd->hc[chnum].do_ping == 0U)
    {
      if (hhcd->hc[chnum].speed == HCD_DEVICE_SPEED_HIGH)
      {
        hhcd->hc[chnum].do_ping = 1U;
      }
    }

    (void)USB_HC_Halt(hhcd->Instance, chnum);
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_NAK);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_TXERR))
  {
    if (hhcd->Init.dma_enable == 0U)
    {
      hhcd->hc[chnum].state = HC_XACTERR;
      (void)USB_HC_Halt(hhcd->Instance, chnum);
    }
    else
    {
      hhcd->hc[chnum].ErrCnt++;
      if (hhcd->hc[chnum].ErrCnt > 2U)
      {
        hhcd->hc[chnum].ErrCnt = 0U;
        hhcd->hc[chnum].urb_state = URB_ERROR;

#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
        hhcd->HC_NotifyURBChangeCallback(hhcd, chnum, hhcd->hc[chnum].urb_state);
#else
        HAL_HCD_HC_NotifyURBChange_Callback(hhcd, chnum, hhcd->hc[chnum].urb_state);
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */
      }
      else
      {
        hhcd->hc[chnum].urb_state = URB_NOTREADY;
        HCD_LogNotReady(hhcd, chnum, raw_hcint, raw_hcintmsk);

        /* Re-activate the channel  */
        tmpreg = USBx_HC(chnum)->HCCHAR;
        tmpreg &= ~USB_OTG_HCCHAR_CHDIS;
        tmpreg |= USB_OTG_HCCHAR_CHENA;
        USBx_HC(chnum)->HCCHAR = tmpreg;
      }
    }
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_TXERR);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_DTERR))
  {
    hhcd->hc[chnum].state = HC_DATATGLERR;
    (void)USB_HC_Halt(hhcd->Instance, chnum);
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_DTERR);
  }
  else if (__HAL_HCD_GET_CH_FLAG(hhcd, chnum, USB_OTG_HCINT_CHH))
  {
    __HAL_HCD_CLEAR_HC_INT(chnum, USB_OTG_HCINT_CHH);

    if (hhcd->hc[chnum].state == HC_XFRC)
    {
      hhcd->hc[chnum].state = HC_HALTED;
      hhcd->hc[chnum].urb_state = URB_DONE;

      if ((hhcd->hc[chnum].ep_type == EP_TYPE_BULK) ||
          (hhcd->hc[chnum].ep_type == EP_TYPE_INTR))
      {
        if (hhcd->Init.dma_enable == 0U)
        {
          hhcd->hc[chnum].toggle_out ^= 1U;
        }

        if ((hhcd->Init.dma_enable == 1U) && (hhcd->hc[chnum].xfer_len > 0U))
        {
          num_packets = (hhcd->hc[chnum].xfer_len + hhcd->hc[chnum].max_packet - 1U) / hhcd->hc[chnum].max_packet;

          if ((num_packets & 1U) != 0U)
          {
            hhcd->hc[chnum].toggle_out ^= 1U;
          }
        }
      }
    }
    else if (hhcd->hc[chnum].state == HC_ACK)
    {
      hhcd->hc[chnum].state = HC_HALTED;

      if (hhcd->hc[chnum].do_csplit == 1U)
      {
        hhcd->hc[chnum].urb_state = URB_NOTREADY;
      }
    }
    else if (hhcd->hc[chnum].state == HC_NAK)
    {
      hhcd->hc[chnum].state = HC_HALTED;
      hhcd->hc[chnum].urb_state = URB_NOTREADY;

      if (hhcd->hc[chnum].do_csplit == 1U)
      {
        hhcd->hc[chnum].do_csplit = 0U;
        __HAL_HCD_CLEAR_HC_CSPLT(chnum);
      }
    }
    else if (hhcd->hc[chnum].state == HC_NYET)
    {
      hhcd->hc[chnum].state = HC_HALTED;
      hhcd->hc[chnum].urb_state  = URB_NOTREADY;
    }
    else if (hhcd->hc[chnum].state == HC_STALL)
    {
      hhcd->hc[chnum].state = HC_HALTED;
      hhcd->hc[chnum].urb_state  = URB_STALL;
    }
    else if ((hhcd->hc[chnum].state == HC_XACTERR) ||
             (hhcd->hc[chnum].state == HC_DATATGLERR))
    {
      hhcd->hc[chnum].state = HC_HALTED;
      hhcd->hc[chnum].ErrCnt++;
      if (hhcd->hc[chnum].ErrCnt > 2U)
      {
        hhcd->hc[chnum].ErrCnt = 0U;
        hhcd->hc[chnum].urb_state = URB_ERROR;
      }
      else
      {
        hhcd->hc[chnum].urb_state = URB_NOTREADY;

        /* re-activate the channel  */
        tmpreg = USBx_HC(chnum)->HCCHAR;
        tmpreg &= ~USB_OTG_HCCHAR_CHDIS;
        tmpreg |= USB_OTG_HCCHAR_CHENA;
        USBx_HC(chnum)->HCCHAR = tmpreg;
      }
    }
    else
    {
      return;
    }

    HCD_LogCHH(hhcd, chnum, 0U, (uint32_t)prev_state, raw_hcint, raw_hcintmsk);

    if (hhcd->hc[chnum].urb_state == URB_NOTREADY)
    {
      HCD_LogNotReady(hhcd, chnum, raw_hcint, raw_hcintmsk);
    }

#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
    hhcd->HC_NotifyURBChangeCallback(hhcd, chnum, hhcd->hc[chnum].urb_state);
#else
    HAL_HCD_HC_NotifyURBChange_Callback(hhcd, chnum, hhcd->hc[chnum].urb_state);
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */
  }
  else
  {
    return;
  }
}

/**
  * @brief  Handle Rx Queue Level interrupt requests.
  * @param  hhcd HCD handle
  * @retval none
  */
static void HCD_RXQLVL_IRQHandler(HCD_HandleTypeDef *hhcd)
{
  const USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
  uint32_t USBx_BASE = (uint32_t)USBx;
  uint32_t pktsts;
  uint32_t pktcnt;
  uint32_t GrxstspReg;
  uint32_t xferSizePktCnt;
  uint32_t tmpreg;
  uint32_t chnum;

  GrxstspReg = hhcd->Instance->GRXSTSP;
  chnum = GrxstspReg & USB_OTG_GRXSTSP_EPNUM;
  pktsts = (GrxstspReg & USB_OTG_GRXSTSP_PKTSTS) >> 17;
  pktcnt = (GrxstspReg & USB_OTG_GRXSTSP_BCNT) >> 4;

  switch (pktsts)
  {
    case GRXSTS_PKTSTS_IN:
      /* Read the data into the host buffer. */
      if ((pktcnt > 0U) && (hhcd->hc[chnum].xfer_buff != (void *)0))
      {
        if ((hhcd->hc[chnum].xfer_count + pktcnt) <= hhcd->hc[chnum].xfer_len)
        {
          (void)USB_ReadPacket(hhcd->Instance,
                               hhcd->hc[chnum].xfer_buff, (uint16_t)pktcnt);

          /* manage multiple Xfer */
          hhcd->hc[chnum].xfer_buff += pktcnt;
          hhcd->hc[chnum].xfer_count += pktcnt;

          /* get transfer size packet count */
          xferSizePktCnt = (USBx_HC(chnum)->HCTSIZ & USB_OTG_HCTSIZ_PKTCNT) >> 19;

          if ((hhcd->hc[chnum].max_packet == pktcnt) && (xferSizePktCnt > 0U))
          {
            /* re-activate the channel when more packets are expected */
            tmpreg = USBx_HC(chnum)->HCCHAR;
            tmpreg &= ~USB_OTG_HCCHAR_CHDIS;
            tmpreg |= USB_OTG_HCCHAR_CHENA;
            USBx_HC(chnum)->HCCHAR = tmpreg;
            hhcd->hc[chnum].toggle_in ^= 1U;
          }
        }
        else
        {
          hhcd->hc[chnum].urb_state = URB_ERROR;
        }
      }
      break;

    case GRXSTS_PKTSTS_DATA_TOGGLE_ERR:
      break;

    case GRXSTS_PKTSTS_IN_XFER_COMP:
    case GRXSTS_PKTSTS_CH_HALTED:
    default:
      break;
  }
}

/**
  * @brief  Handle Host Port interrupt requests.
  * @param  hhcd HCD handle
  * @retval None
  */
static void HCD_Port_IRQHandler(HCD_HandleTypeDef *hhcd)
{
  const USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
  uint32_t USBx_BASE = (uint32_t)USBx;
  __IO uint32_t hprt0;
  __IO uint32_t hprt0_dup;

  /* Handle Host Port Interrupts */
  hprt0 = USBx_HPRT0;
  hprt0_dup = USBx_HPRT0;

  hprt0_dup &= ~(USB_OTG_HPRT_PENA | USB_OTG_HPRT_PCDET | \
                 USB_OTG_HPRT_PENCHNG | USB_OTG_HPRT_POCCHNG);

  /* Check whether Port Connect detected */
  if ((hprt0 & USB_OTG_HPRT_PCDET) == USB_OTG_HPRT_PCDET)
  {
    if ((hprt0 & USB_OTG_HPRT_PCSTS) == USB_OTG_HPRT_PCSTS)
    {
#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
      hhcd->ConnectCallback(hhcd);
#else
      HAL_HCD_Connect_Callback(hhcd);
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */
    }
    hprt0_dup |= USB_OTG_HPRT_PCDET;
  }

  /* Check whether Port Enable Changed */
  if ((hprt0 & USB_OTG_HPRT_PENCHNG) == USB_OTG_HPRT_PENCHNG)
  {
    hprt0_dup |= USB_OTG_HPRT_PENCHNG;

    if ((hprt0 & USB_OTG_HPRT_PENA) == USB_OTG_HPRT_PENA)
    {
      if (hhcd->Init.phy_itface == USB_OTG_EMBEDDED_PHY)
      {
        if ((hprt0 & USB_OTG_HPRT_PSPD) == (HPRT0_PRTSPD_LOW_SPEED << 17))
        {
          (void)USB_InitFSLSPClkSel(hhcd->Instance, HCFG_6_MHZ);
        }
        else
        {
          (void)USB_InitFSLSPClkSel(hhcd->Instance, HCFG_48_MHZ);
        }
      }
      else
      {
        if (hhcd->Init.speed == HCD_SPEED_FULL)
        {
          USBx_HOST->HFIR = HFIR_60_MHZ;
        }
      }
#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
      hhcd->PortEnabledCallback(hhcd);
#else
      HAL_HCD_PortEnabled_Callback(hhcd);
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */

    }
    else
    {
#if (USE_HAL_HCD_REGISTER_CALLBACKS == 1U)
      hhcd->PortDisabledCallback(hhcd);
#else
      HAL_HCD_PortDisabled_Callback(hhcd);
#endif /* USE_HAL_HCD_REGISTER_CALLBACKS */
    }
  }

  /* Check for an overcurrent */
  if ((hprt0 & USB_OTG_HPRT_POCCHNG) == USB_OTG_HPRT_POCCHNG)
  {
    hprt0_dup |= USB_OTG_HPRT_POCCHNG;
  }

  /* Clear Port Interrupts */
  USBx_HPRT0 = hprt0_dup;
}

/**
  * @}
  */

/**
  * @}
  */

#endif /* defined (USB_OTG_FS) || defined (USB_OTG_HS) */
#endif /* HAL_HCD_MODULE_ENABLED */

/**
  * @}
  */

/**
  * @}
  */
