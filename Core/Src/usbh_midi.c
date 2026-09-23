/**
 ******************************************************************************
 * @file    usbh_midi.c
 * @brief   This file is the MIDI Layer Handlers for USB Host MIDI streaming class.
 *
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "usbh_midi.h"
#include <stdio.h>

/*------------------------------------------------------------------------------------------------------------------------------*/

/** @defgroup USBH_MIDI_CORE_Private_FunctionPrototypes
 * @{
 */

static USBH_StatusTypeDef USBH_MIDI_InterfaceInit  (USBH_HandleTypeDef *phost);

static USBH_StatusTypeDef USBH_MIDI_InterfaceDeInit  (USBH_HandleTypeDef *phost);

static USBH_StatusTypeDef USBH_MIDI_Process(USBH_HandleTypeDef *phost);

static USBH_StatusTypeDef USBH_MIDI_SOFProcess(USBH_HandleTypeDef *phost);

static USBH_StatusTypeDef USBH_MIDI_ClassRequest (USBH_HandleTypeDef *phost);

static void MIDI_ProcessTransmission(USBH_HandleTypeDef *phost);

static void MIDI_ProcessReception(USBH_HandleTypeDef *phost);

/*-------------------------------------------------------------------------*/

USBH_ClassTypeDef  MIDI_Class =
{
		"MIDI",
		USB_AUDIO_CLASS,
		USBH_MIDI_InterfaceInit,
		USBH_MIDI_InterfaceDeInit,
		USBH_MIDI_ClassRequest,
		USBH_MIDI_Process, // background process called in HOST_CLASS state (core state machine)
		USBH_MIDI_SOFProcess,
		NULL // MIDI handle structure
};

/*------------------------------------------------------------------------------------------------------------------------------*/
/**
 * @brief  USBH_MIDI_InterfaceInit
 *         The function init the MIDI class.
 * @param  phost: Host handle
 * @retval USBH Status
 */
static USBH_StatusTypeDef USBH_MIDI_InterfaceInit (USBH_HandleTypeDef *phost)
{
	USBH_StatusTypeDef status = USBH_FAIL;
	uint8_t interface = 0;
	MIDI_HandleTypeDef *MIDI_Handle;

	interface = USBH_FindInterface(phost, USB_AUDIO_CLASS, USB_MIDISTREAMING_SubCLASS, 0xFF);

	if(interface == 0xFF) /* No Valid Interface */
	{
		USBH_DbgLog ("Cannot Find the interface for MIDI Interface Class.", phost->pActiveClass->Name);
		status = USBH_FAIL;
	}
	else
	{
		USBH_SelectInterface (phost, interface);

		phost->pActiveClass->pData = (MIDI_HandleTypeDef *)USBH_malloc (sizeof(MIDI_HandleTypeDef));
		MIDI_Handle = (MIDI_HandleTypeDef *)phost->pActiveClass->pData;

		if (MIDI_Handle == NULL)
		{
			USBH_DbgLog("Cannot allocate memory for MIDI Handle");
			return USBH_FAIL;
		}

		USBH_memset(MIDI_Handle, 0, sizeof(MIDI_HandleTypeDef)); // clear memory for MIDI_Handle

		// Находим реальное количество конечных точек в MIDI интерфейсе
        MIDI_Handle->InEp = 0;
        MIDI_Handle->OutEp = 0;

        uint8_t in_ep_type = USB_EP_TYPE_BULK;
        uint8_t out_ep_type = USB_EP_TYPE_BULK;

        // ВМЕСТО перебора всех интерфейсов, смотрим ТОЛЬКО в найденный MIDI-интерфейс (переменная interface)
        uint8_t num_ep = phost->device.CfgDesc.Itf_Desc[interface].bNumEndpoints;

        for (uint8_t i = 0; i < num_ep; i++) {
            uint8_t ep_addr = phost->device.CfgDesc.Itf_Desc[interface].Ep_Desc[i].bEndpointAddress;
            uint16_t ep_size = phost->device.CfgDesc.Itf_Desc[interface].Ep_Desc[i].wMaxPacketSize;

            uint8_t current_ep_type = phost->device.CfgDesc.Itf_Desc[interface].Ep_Desc[i].bmAttributes & 0x03U;

            // Теперь можно смело разрешать INTR, так как мы ищем только внутри MIDI-интерфейса
            if (current_ep_type == USB_EP_TYPE_BULK || current_ep_type == USB_EP_TYPE_INTR) {
                if ((ep_addr & 0x80U) != 0U) {
                    if (MIDI_Handle->InEp == 0U) {
                        MIDI_Handle->InEp = ep_addr;
                        MIDI_Handle->InEpSize = ep_size;
                        MIDI_Handle->InEpType = current_ep_type;
                        in_ep_type = current_ep_type;
                    }
                } else {
                    if (MIDI_Handle->OutEp == 0U) {
                        MIDI_Handle->OutEp = ep_addr;
                        MIDI_Handle->OutEpSize = ep_size;
                        MIDI_Handle->OutEpType = current_ep_type;
                        out_ep_type = current_ep_type;
                    }
                }
            }
		}

		if (MIDI_Handle->InEp == 0)
		{
			USBH_DbgLog("Cannot Find the MIDI IN Endpoint");
			USBH_free(MIDI_Handle);
			phost->pActiveClass->pData = 0;
			return USBH_FAIL;
		}

		// Выделяем и открываем каналы, передавая сохраненные типы
		if (MIDI_Handle->OutEp != 0) {
			MIDI_Handle->OutPipe = USBH_AllocPipe(phost, MIDI_Handle->OutEp);
			if (MIDI_Handle->OutPipe != 0xFFFFU && MIDI_Handle->OutPipe != 0) {
				if (USBH_OpenPipe(phost, MIDI_Handle->OutPipe, MIDI_Handle->OutEp, phost->device.address, phost->device.speed, out_ep_type, MIDI_Handle->OutEpSize) != USBH_OK) {
					USBH_DbgLog("Cannot open pipe for MIDI OUT Endpoint");
					USBH_FreePipe(phost, MIDI_Handle->OutPipe);
					USBH_free(MIDI_Handle);
					phost->pActiveClass->pData = 0;
					return USBH_FAIL;
				}
				USBH_LL_SetToggle(phost, MIDI_Handle->OutPipe, 0);
			} else {
				USBH_DbgLog("Cannot allocate pipe for MIDI OUT Endpoint");
				USBH_free(MIDI_Handle);
				phost->pActiveClass->pData = 0;
				return USBH_FAIL;
			}
		}

		if (MIDI_Handle->InEp != 0) {
			MIDI_Handle->InPipe = USBH_AllocPipe(phost, MIDI_Handle->InEp);
			if (MIDI_Handle->InPipe != 0xFFFFU && MIDI_Handle->InPipe != 0) {
				if (USBH_OpenPipe(phost, MIDI_Handle->InPipe, MIDI_Handle->InEp, phost->device.address, phost->device.speed, in_ep_type, MIDI_Handle->InEpSize) != USBH_OK) {
					USBH_DbgLog("Cannot open pipe for MIDI IN Endpoint");
					if (MIDI_Handle->OutPipe != 0xFFFFU && MIDI_Handle->OutPipe != 0) {
						USBH_ClosePipe(phost, MIDI_Handle->OutPipe);
						USBH_FreePipe(phost, MIDI_Handle->OutPipe);
					}
					USBH_FreePipe(phost, MIDI_Handle->InPipe);
					USBH_free(MIDI_Handle);
					phost->pActiveClass->pData = 0;
					return USBH_FAIL;
				}
				USBH_LL_SetToggle(phost, MIDI_Handle->InPipe, 0);
			} else {
				USBH_DbgLog("Cannot allocate pipe for MIDI IN Endpoint");
				if (MIDI_Handle->OutPipe != 0xFFFFU && MIDI_Handle->OutPipe != 0) {
					USBH_ClosePipe(phost, MIDI_Handle->OutPipe);
					USBH_FreePipe(phost, MIDI_Handle->OutPipe);
				}
				USBH_free(MIDI_Handle);
				phost->pActiveClass->pData = 0;
				return USBH_FAIL;
			}
		}

		MIDI_Handle->state = MIDI_IDLE_STATE;
		status = USBH_OK;
	}
	return status;
}
/*------------------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief  USBH_MIDI_InterfaceDeInit
 *         The function DeInit the Pipes used for the MIDI class.
 * @param  phost: Host handle
 * @retval USBH Status
 */
USBH_StatusTypeDef USBH_MIDI_InterfaceDeInit (USBH_HandleTypeDef *phost)
{
	if (phost == NULL || phost->pActiveClass == NULL || phost->pActiveClass->pData == NULL) {
		return USBH_FAIL;
	}

	MIDI_HandleTypeDef *MIDI_Handle =  phost->pActiveClass->pData;

	if ( MIDI_Handle->OutPipe)
	{
		USBH_ClosePipe(phost, MIDI_Handle->OutPipe);
		USBH_FreePipe  (phost, MIDI_Handle->OutPipe);
		MIDI_Handle->OutPipe = 0;     /* Reset the Channel as Free */
	}

	if ( MIDI_Handle->InPipe)
	{
		USBH_ClosePipe(phost, MIDI_Handle->InPipe);
		USBH_FreePipe  (phost, MIDI_Handle->InPipe);
		MIDI_Handle->InPipe = 0;     /* Reset the Channel as Free */
	}

	if(phost->pActiveClass->pData)
	{
		USBH_free (phost->pActiveClass->pData);
		phost->pActiveClass->pData = 0;
	}

	return USBH_OK;
}

/*------------------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief  USBH_MIDI_ClassRequest
 *         The function is responsible for handling Standard requests
 *         for MIDI class.
 * @param  phost: Host handle
 * @retval USBH Status
 */
static USBH_StatusTypeDef USBH_MIDI_ClassRequest (USBH_HandleTypeDef *phost)
{

	phost->pUser(phost, HOST_USER_CLASS_ACTIVE);

	return USBH_OK;
}

/*------------------------------------------------------------------------------------------------------------------------------*/

/**
  * @brief  USBH_MIDI_Stop
  *         Stop current MIDI Transmission
  * @param  phost: Host handle
  * @retval USBH Status
  */
USBH_StatusTypeDef  USBH_MIDI_Stop(USBH_HandleTypeDef *phost)
{
	if (phost == NULL || phost->pActiveClass == NULL || phost->pActiveClass->pData == NULL) {
		return USBH_FAIL;
	}

  MIDI_HandleTypeDef *MIDI_Handle =  phost->pActiveClass->pData;

  if(phost->gState == HOST_CLASS)
  {
    MIDI_Handle->state = MIDI_IDLE_STATE;

    USBH_ClosePipe(phost, MIDI_Handle->InPipe);
    USBH_ClosePipe(phost, MIDI_Handle->OutPipe);
  }
  return USBH_OK;
}

/*------------------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief  USBH_MIDI_Process
 *         The function is for managing state machine for MIDI data transfers
 *         (background process)
 * @param  phost: Host handle
 * @retval USBH Status
 */
static USBH_StatusTypeDef USBH_MIDI_Process (USBH_HandleTypeDef *phost)
{
	USBH_StatusTypeDef status = USBH_BUSY;
	USBH_StatusTypeDef req_status = USBH_OK;
	MIDI_HandleTypeDef *MIDI_Handle =  phost->pActiveClass->pData;

	switch(MIDI_Handle->state)
	{

	case MIDI_IDLE_STATE:
		status = USBH_OK;
		break;

	case MIDI_TRANSFER_DATA:

		MIDI_ProcessTransmission(phost);
		MIDI_ProcessReception(phost);
		break;

	case MIDI_ERROR_STATE:
		req_status = USBH_ClrFeature(phost, 0x00);

		if(req_status == USBH_OK )
		{
			/*Change the state to waiting*/
			MIDI_Handle->state = MIDI_IDLE_STATE ;
		}
		break;

	default:
		break;

	}

	return status;
}
/*------------------------------------------------------------------------------------------------------------------------------*/

/**
  * @brief  USBH_MIDI_SOFProcess
  *         The function is for managing SOF callback
  * @param  phost: Host handle
  * @retval USBH Status
  */
static USBH_StatusTypeDef USBH_MIDI_SOFProcess (USBH_HandleTypeDef *phost)
{
  return USBH_OK;
}

/*------------------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief  This function return last recieved data size
 * @param  None
 * @retval None
 */
uint16_t USBH_MIDI_GetLastReceivedDataSize(USBH_HandleTypeDef *phost)
{
	if (phost == NULL || phost->pActiveClass == NULL || phost->pActiveClass->pData == NULL) {
		return 0;
	}

	MIDI_HandleTypeDef *MIDI_Handle =  phost->pActiveClass->pData;

	if(phost->gState == HOST_CLASS)
	{
		return MIDI_Handle->LastRxLength;
	}
	else
	{
		return 0;
	}
}

/*------------------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief  This function prepares the state before issuing the class specific commands
 * @param  None
 * @retval None
 */
USBH_StatusTypeDef  USBH_MIDI_Transmit(USBH_HandleTypeDef *phost, uint8_t *pbuff, uint16_t length)
{
	USBH_StatusTypeDef Status = USBH_BUSY;
	if (phost == NULL || phost->pActiveClass == NULL || phost->pActiveClass->pData == NULL) {
		return USBH_FAIL;
	}

	MIDI_HandleTypeDef *MIDI_Handle =  phost->pActiveClass->pData;

	if (MIDI_Handle->OutPipe == 0 || MIDI_Handle->OutEpSize == 0 || pbuff == NULL || length == 0) {
		return USBH_FAIL;
	}

	if((MIDI_Handle->state == MIDI_IDLE_STATE) || (MIDI_Handle->state == MIDI_TRANSFER_DATA))
	{
		if (MIDI_Handle->data_tx_state == MIDI_IDLE) {
			MIDI_Handle->pTxData = pbuff;
			MIDI_Handle->TxDataLength = length;
			MIDI_Handle->state = MIDI_TRANSFER_DATA;
			MIDI_Handle->data_tx_state = MIDI_SEND_DATA;
			Status = USBH_OK;
#if (USBH_USE_OS == 1)
			osMessagePut ( phost->os_event, USBH_CLASS_EVENT, 0);
#endif
		}
	}
	return Status;
}

/*------------------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief  This function prepares the state before issuing the class specific commands
 * @param  None
 * @retval None
 */
USBH_StatusTypeDef  USBH_MIDI_Receive(USBH_HandleTypeDef *phost, uint8_t *pbuff, uint16_t length)
{
	USBH_StatusTypeDef Status = USBH_BUSY;
	if (phost == NULL || phost->pActiveClass == NULL || phost->pActiveClass->pData == NULL) {
		return USBH_FAIL;
	}

	MIDI_HandleTypeDef *MIDI_Handle =  phost->pActiveClass->pData;

	if (pbuff == NULL || length == 0) {
		return USBH_FAIL;
	}

	if((MIDI_Handle->state == MIDI_IDLE_STATE) || (MIDI_Handle->state == MIDI_TRANSFER_DATA))
	{
		if (MIDI_Handle->data_rx_state == MIDI_IDLE) {
			MIDI_Handle->pRxData = pbuff;
			MIDI_Handle->RxDataLength = length;
			MIDI_Handle->state = MIDI_TRANSFER_DATA;
			MIDI_Handle->data_rx_state = MIDI_RECEIVE_DATA;
			Status = USBH_OK;
#if (USBH_USE_OS == 1)
			osMessagePut ( phost->os_event, USBH_CLASS_EVENT, 0);
#endif
		}
	}
	return Status;
}

/*------------------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief  The function is responsible for sending data to the device
 *  @param  pdev: Selected device
 * @retval None
 */
static void MIDI_ProcessTransmission(USBH_HandleTypeDef *phost)
{
	MIDI_HandleTypeDef *MIDI_Handle =  phost->pActiveClass->pData;
	USBH_URBStateTypeDef URB_Status = USBH_URB_IDLE;

	switch(MIDI_Handle->data_tx_state)
	{

	case MIDI_SEND_DATA:
		if (MIDI_Handle->OutEpType == USB_EP_TYPE_INTR)
		{
			if(MIDI_Handle->TxDataLength > MIDI_Handle->OutEpSize)
			{
				USBH_InterruptSendData (phost,
						MIDI_Handle->pTxData,
						MIDI_Handle->OutEpSize,
						MIDI_Handle->OutPipe);
			}
			else
			{
				USBH_InterruptSendData (phost,
						MIDI_Handle->pTxData,
						MIDI_Handle->TxDataLength,
						MIDI_Handle->OutPipe);
			}
		}
		else
		{
			if(MIDI_Handle->TxDataLength > MIDI_Handle->OutEpSize)
			{
				USBH_BulkSendData (phost,
						MIDI_Handle->pTxData,
						MIDI_Handle->OutEpSize,
						MIDI_Handle->OutPipe,
						1);
			}
			else
			{
				USBH_BulkSendData (phost,
						MIDI_Handle->pTxData,
						MIDI_Handle->TxDataLength,
						MIDI_Handle->OutPipe,
						1);
			}
		}

		MIDI_Handle->data_tx_state = MIDI_SEND_DATA_WAIT;

		break;

	case MIDI_SEND_DATA_WAIT:

		URB_Status = USBH_LL_GetURBState(phost, MIDI_Handle->OutPipe);

		/*Check the status done for transmission*/
		if(URB_Status == USBH_URB_DONE )
		{
			if(MIDI_Handle->TxDataLength > MIDI_Handle->OutEpSize)
			{
				MIDI_Handle->TxDataLength -= MIDI_Handle->OutEpSize ;
				MIDI_Handle->pTxData += MIDI_Handle->OutEpSize;
			}
			else
			{
				MIDI_Handle->TxDataLength = 0;
			}

			if( MIDI_Handle->TxDataLength > 0)
			{
				MIDI_Handle->data_tx_state = MIDI_SEND_DATA;
			}
			else
			{
				MIDI_Handle->data_tx_state = MIDI_IDLE;
				USBH_MIDI_TransmitCallback(phost);
			}
#if (USBH_USE_OS == 1)
			osMessagePut ( phost->os_event, USBH_CLASS_EVENT, 0);
#endif
		}
		else if( URB_Status == USBH_URB_NOTREADY )
		{
			MIDI_Handle->data_tx_state = MIDI_SEND_DATA;
#if (USBH_USE_OS == 1)
			osMessagePut ( phost->os_event, USBH_CLASS_EVENT, 0);
#endif
		}
		else if (URB_Status == USBH_URB_ERROR || URB_Status == USBH_URB_STALL)
		{
			MIDI_Handle->data_tx_state = MIDI_IDLE;
			USBH_MIDI_TransmitErrorCallback(phost);
		}
		break;
	default:
		break;
	}
}

/*------------------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief  This function responsible for reception of data from the device
 *  @param  pdev: Selected device
 * @retval None
 */

static void MIDI_ProcessReception(USBH_HandleTypeDef *phost)
{
    MIDI_HandleTypeDef *MIDI_Handle =  phost->pActiveClass->pData;
    USBH_URBStateTypeDef URB_Status = USBH_URB_IDLE;
    // uint16_t length;

    switch(MIDI_Handle->data_rx_state)
    {
    case MIDI_RECEIVE_DATA:
		{
			uint16_t transfer_size = MIDI_Handle->RxDataLength;
			if (transfer_size > MIDI_Handle->InEpSize)
			{
				transfer_size = MIDI_Handle->InEpSize;
			}
			if (transfer_size == 0)
			{
				MIDI_Handle->data_rx_state = MIDI_IDLE;
				break;
			}
			// 1. Отправляем запрос на чтение в зависимости от типа конечной точки
			if (MIDI_Handle->InEpType == USB_EP_TYPE_INTR)
			{
				USBH_InterruptReceiveData(phost,
						MIDI_Handle->pRxData,
						transfer_size,
						MIDI_Handle->InPipe);
			}
			else
			{
				USBH_BulkReceiveData (phost,
						MIDI_Handle->pRxData,
						transfer_size,
						MIDI_Handle->InPipe);
			}
			MIDI_Handle->data_rx_state = MIDI_RECEIVE_DATA_WAIT;
		}
        break;

    case MIDI_RECEIVE_DATA_WAIT:
        URB_Status = USBH_LL_GetURBState(phost, MIDI_Handle->InPipe);

        if (URB_Status == USBH_URB_DONE)
        {
            MIDI_Handle->LastRxLength = USBH_LL_GetLastXferSize(phost, MIDI_Handle->InPipe);
            MIDI_Handle->data_rx_state = MIDI_IDLE;
            USBH_MIDI_ReceiveCallback(phost);
            // Если пакет прочитан успешно, callback запустит чтение заново.
            // Благодаря отсутствию задержки, следующий пакет вытянется моментально.
        }
		else if (URB_Status == USBH_URB_NOTREADY)
        {
            if (MIDI_Handle->InEpType == USB_EP_TYPE_INTR)
            {
			    // Независимый таймер вместо аппаратного флага SOF,
                // который отключается при некоторых генерациях CubeMX
                static uint32_t last_nak_time = 0;
                if (HAL_GetTick() - last_nak_time >= 1)
                {
                    last_nak_time = HAL_GetTick();
                    MIDI_Handle->data_rx_state = MIDI_RECEIVE_DATA;
                }
            }
			else
			{
				// For Bulk endpoint after USBH_URB_NOTREADY we should resubmit
				MIDI_Handle->data_rx_state = MIDI_RECEIVE_DATA;
			}
        }
        else if (URB_Status == USBH_URB_STALL)
        {
			MIDI_Handle->RxStallRetryCounter = 3; /* Limit retries */
            MIDI_Handle->data_rx_state = MIDI_RECEIVE_DATA_WAIT_STALL;
        }
        else if (URB_Status == USBH_URB_ERROR)
        {
            MIDI_Handle->data_rx_state = MIDI_RECEIVE_DATA;
        }
        break;

	case MIDI_RECEIVE_DATA_WAIT_STALL:
		{
			USBH_StatusTypeDef clr_status = USBH_ClrFeature(phost, MIDI_Handle->InEp);
			if (clr_status == USBH_OK)
			{
				USBH_LL_SetToggle(phost, MIDI_Handle->InPipe, 0);
				MIDI_Handle->data_rx_state = MIDI_RECEIVE_DATA;
			}
			else if (clr_status != USBH_BUSY)
			{
				if (--MIDI_Handle->RxStallRetryCounter == 0)
				{
					MIDI_Handle->data_rx_state = MIDI_IDLE;
				}
			}
		}
		break;

    default:
        break;
    }
}

/*------------------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief  The function informs user that data have been transmitted.
 *  @param  pdev: Selected device
 * @retval None
 */
__weak void USBH_MIDI_TransmitCallback(USBH_HandleTypeDef *phost)
{

}

/**
 * @brief  The function informs user that an error occurred during data transmission.
 *  @param  pdev: Selected device
 * @retval None
 */
__weak void USBH_MIDI_TransmitErrorCallback(USBH_HandleTypeDef *phost)
{

}

/*------------------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief  The function informs user that data have been received.
 * @retval None
 */
__weak void USBH_MIDI_ReceiveCallback(USBH_HandleTypeDef *phost)
{

}

/**************************END OF FILE*********************************************************/
