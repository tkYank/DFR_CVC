/*
 * cvc_sd.c
 *
 *  Created on: Dec 9, 2018
 *      Author: f002bc7
 */

#include "cvc_sd.h"

/* Standard includes. */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* FreeRTOS Includes */
#include "FreeRTOS.h"

/* ST HAL includes. */
#include "stm32f7xx_hal.h"
#include "stm32f7xx_hal_sd.h"

/* Configuration Defines */
#define BUS_4BITS							1
#define configSD_DETECT_PIN					GPIO_PIN_0
#define configSD_DETECT_GPIO_PORT			GPIOG

/* Misc definitions. */
#define sdSIGNATURE 			0x41404342UL
#define sdHUNDRED_64_BIT		( 100ull )
#define sdBYTES_PER_MB			( 1024ull * 1024ull )
#define sdSECTORS_PER_MB		( sdBYTES_PER_MB / 512ull )
#define sdIOMAN_MEM_SIZE		4096

/* DMA constants. */
#define SD_DMAx_Tx_CHANNEL					DMA_CHANNEL_4
#define SD_DMAx_Rx_CHANNEL					DMA_CHANNEL_4
#define SD_DMAx_Tx_STREAM					DMA2_Stream6
#define SD_DMAx_Rx_STREAM					DMA2_Stream3
#define SD_DMAx_Tx_IRQn						DMA2_Stream6_IRQn
#define SD_DMAx_Rx_IRQn						DMA2_Stream3_IRQn
#define __DMAx_TxRx_CLK_ENABLE				__DMA2_CLK_ENABLE
#define configSDIO_DMA_INTERRUPT_PRIORITY	( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY )

/* Define a time-out for all DMA transactions in msec. */
#ifndef sdMAX_TIME_TICKS
	#define sdMAX_TIME_TICKS	pdMS_TO_TICKS( 2000UL )
#endif

#ifndef configSD_DETECT_PIN
	#error configSD_DETECT_PIN must be defined in FreeRTOSConfig.h to the pin used to detect if the SD card is present.
#endif

#ifndef configSD_DETECT_GPIO_PORT
	#error configSD_DETECT_GPIO_PORT must be defined in FreeRTOSConfig.h to the port on which configSD_DETECT_PIN is located.
#endif

#ifndef sdCARD_DETECT_DEBOUNCE_TIME_MS
	/* Debouncing time is applied only after card gets inserted. */
	#define sdCARD_DETECT_DEBOUNCE_TIME_MS	( 5000 )
#endif

#ifndef sdARRAY_SIZE
	#define	sdARRAY_SIZE( x )	( int )( sizeof( x ) / sizeof( x )[ 0 ] )
#endif


/* The Instance of the MMC peripheral. */
#define	SDIO	SDMMC1

#ifdef GPIO_AF12_SDIO
	#undef GPIO_AF12_SDIO
#endif
#define GPIO_AF12_SDIO						GPIO_AF12_SDMMC1

#define SDIO_CLOCK_EDGE_RISING				SDMMC_CLOCK_EDGE_RISING
#define SDIO_CLOCK_BYPASS_DISABLE			SDMMC_CLOCK_BYPASS_DISABLE
#define SDIO_CLOCK_POWER_SAVE_DISABLE		SDMMC_CLOCK_POWER_SAVE_DISABLE
#define SDIO_BUS_WIDE_1B					SDMMC_BUS_WIDE_1B
#define SDIO_BUS_WIDE_4B					SDMMC_BUS_WIDE_4B
#define SDIO_HARDWARE_FLOW_CONTROL_DISABLE	SDMMC_HARDWARE_FLOW_CONTROL_DISABLE


#define SD_SDIO_DISABLED					SD_SDMMC_DISABLED
#define SD_SDIO_FUNCTION_BUSY				SD_SDMMC_FUNCTION_BUSY
#define SD_SDIO_FUNCTION_FAILED				SD_SDMMC_FUNCTION_FAILED
#define SD_SDIO_UNKNOWN_FUNCTION			SD_SDMMC_UNKNOWN_FUNCTION

#define SDIO_IRQn							SDMMC1_IRQn


/* Private Variables */
static SD_HandleTypeDef SDHandle;

/* Private Function Prototypes */
static void GPIO_SD_Init(SD_HandleTypeDef* hsp);
static void SDIO_SD_Init( void );
static BaseType_t SDDetect( void );
static void SDIO_DMA_Init( void );

/*-----------------------------------------------------------*/

/**
  * @brief  Returns information about specific card.
  * @param  pCardInfo: Pointer to a SD_CardInfo structure that contains all SD
  *         card information.
  * @retval The SD Response:
  *         - MSD_ERROR: Sequence failed
  *         - MSD_OK: Sequence succeed
  */
uint8_t BSP_SD_GetCardInfo(BSP_SD_CardInfo *pCardInfo)
{
  uint8_t status;

  status = HAL_SD_GetCardInfo(&SDHandle, pCardInfo);

  return status;
}


/**
  * @brief  Writes block(s) to a specified address in the SD card, in DMA mode.
  * @param  pData: Pointer to the buffer that will contain the data to transmit
  * @param  WriteAddr: Address from where data is to be written. The address is counted
  *                   in blocks of 512bytes
  * @param  NumOfBlocks: Number of SD blocks to write
  * @param  Timeout: This parameter is used for compatibility with BSP implementation
  * @retval SD status
  */
uint8_t BSP_SD_WriteBlocks_DMA(uint8_t *pData, uint32_t WriteAddr, uint32_t NumOfBlocks)
{
	HAL_StatusTypeDef ret = HAL_SD_WriteBlocks_DMA(&SDHandle, pData, WriteAddr, NumOfBlocks);

	return ret;
}
/**
  * @brief  Reads block(s) from a specified address in the SD card, in DMA mode.
  * @param  pData: Pointer to the buffer that will contain the data to transmit
  * @param  ReadAddr: Address from where data is to be read. The address is counted
  *                   in blocks of 512bytes
  * @param  NumOfBlocks: Number of SD blocks to read
  * @param  Timeout: This parameter is used for compatibility with BSP implementation
  * @retval SD status
  */
uint8_t BSP_SD_ReadBlocks_DMA(uint8_t *pData, uint32_t ReadAddr, uint32_t NumOfBlocks)
{
	return HAL_SD_ReadBlocks_DMA(&SDHandle, pData, ReadAddr, NumOfBlocks);
}

/**
  * @brief  Returns the SD transfer status.
  * @param  None
  * @retval The SD transfer status.
  */
uint8_t BSP_SD_GetTransferState(void)
{
	switch (HAL_SD_GetCardState(&SDHandle))
	{
		case HAL_SD_CARD_TRANSFER:
			return SD_TRANSFER_OK;
		case HAL_SD_CARD_ERROR:
			return SD_TRANSFER_ERROR;
		default:
			return SD_TRANSFER_BUSY;
	}
}

/**
  * @brief  Returns the SD status.
  * @param  None
  * @retval The SD status.
  */
uint8_t BSP_SD_GetCardState(void)
{
	HAL_SD_CardStateTypeDef state = HAL_SD_GetCardState(&SDHandle);

	if (state != HAL_SD_CARD_ERROR && state != HAL_SD_CARD_DISCONNECTED) {
		return BSP_SD_OK;
	}
	else
	{
		return BSP_SD_ERROR;
	}
}

uint8_t BSP_SD_Init(void)
{
	/* initialize SDHandle */
	SDIO_SD_Init();

	/* initialize GPIOs for SDIO */
	GPIO_SD_Init(&SDHandle);

	/* Initialize DMA */
	SDIO_DMA_Init( );

	/* Check if SD card is present */
	if( SDDetect() == 0 )
	{
		/* no SD card detected */
		return BSP_SD_ERROR;
	}

	/* Initialize SD card */
	if (HAL_SD_Init(&SDHandle) != HAL_OK)
	{
		/* Error with initialization */
		return BSP_SD_ERROR;
	}

	return BSP_SD_OK;
}

static void GPIO_SD_Init(SD_HandleTypeDef* hsp)
{
GPIO_InitTypeDef GPIO_InitStruct;

	if( hsp->Instance == SDIO )
	{
		/* Peripheral clock enable */
		__SDIO_CLK_ENABLE();

		/**SDIO GPIO Configuration
		PC8     ------> SDIO_D0
		PC9     ------> SDIO_D1
		PC10    ------> SDIO_D2
		PC11    ------> SDIO_D3
		PC12    ------> SDIO_CK
		PD2     ------> SDIO_CMD
		*/

		/* Enable SDIO clock */
		__HAL_RCC_SDMMC1_CLK_ENABLE();

		/* Enable DMA2 clocks */
		__DMAx_TxRx_CLK_ENABLE();

		/* Enable GPIOs clock */
		__HAL_RCC_GPIOC_CLK_ENABLE();
		__HAL_RCC_GPIOD_CLK_ENABLE();

		/* GPIOC configuration */
		#if( BUS_4BITS != 0 )
		{
			GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
		}
		#else
		{
			GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_12;
		}
		#endif
		GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Pull = GPIO_PULLUP;
		GPIO_InitStruct.Speed = GPIO_SPEED_HIGH;
		GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
		HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

		/* GPIOD configuration */
		GPIO_InitStruct.Pin = GPIO_PIN_2;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Pull = GPIO_PULLUP;
		GPIO_InitStruct.Speed = GPIO_SPEED_HIGH;
		GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
		HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);
	}
}

/* SDIO init function */
static void SDIO_SD_Init( void )
{
	SDHandle.Instance = SDIO;
	SDHandle.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
	SDHandle.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
	SDHandle.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;

	/* Set to 4B mode */
	SDHandle.Init.BusWide = SDIO_BUS_WIDE_1B;

	SDHandle.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;

	/* Use fastest CLOCK at 0. */
	SDHandle.Init.ClockDiv = 32;
	__HAL_RCC_SDIO_CLK_ENABLE( );
}

/* Raw SD-card detection, just return the GPIO status. */
static BaseType_t SDDetect( void )
{
int ret;

	/*!< Check GPIO to detect SD */
	if( HAL_GPIO_ReadPin( configSD_DETECT_GPIO_PORT, configSD_DETECT_PIN ) != 0 )
	{
		/* The internal pull-up makes the signal high. */
		ret = 0;
	}
	else
	{
		/* The card will pull the GPIO signal down. */
		ret = 1;
	}

	return ret;
}

static void SDIO_DMA_Init( void )
{
static DMA_HandleTypeDef xRxDMAHandle;
static DMA_HandleTypeDef xTxDMAHandle;

	/* Enable DMA2 clocks */
	__DMAx_TxRx_CLK_ENABLE();

	/* NVIC configuration for SDIO interrupts */
	HAL_NVIC_SetPriority(SDIO_IRQn, configSDIO_DMA_INTERRUPT_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(SDIO_IRQn);

	/* Configure DMA Rx parameters */
	xRxDMAHandle.Init.Channel             = SD_DMAx_Rx_CHANNEL;
	xRxDMAHandle.Init.Direction           = DMA_PERIPH_TO_MEMORY;
	/* Peripheral address is fixed (FIFO). */
	xRxDMAHandle.Init.PeriphInc           = DMA_PINC_DISABLE;
	/* Memory address increases. */
	xRxDMAHandle.Init.MemInc              = DMA_MINC_ENABLE;
	xRxDMAHandle.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
	xRxDMAHandle.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
	/* The peripheral has flow-control. */
	xRxDMAHandle.Init.Mode                = DMA_PFCTRL;
	xRxDMAHandle.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
	xRxDMAHandle.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
	xRxDMAHandle.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
	xRxDMAHandle.Init.MemBurst            = DMA_MBURST_INC4;
	xRxDMAHandle.Init.PeriphBurst         = DMA_PBURST_INC4;

	/* DMA2_Stream3. */
	xRxDMAHandle.Instance = SD_DMAx_Rx_STREAM;

	/* Associate the DMA handle */
	__HAL_LINKDMA(&SDHandle, hdmarx, xRxDMAHandle);

	/* Deinitialize the stream for new transfer */
	HAL_DMA_DeInit(&xRxDMAHandle);

	/* Configure the DMA stream */
	HAL_DMA_Init(&xRxDMAHandle);

	/* Configure DMA Tx parameters */
	xTxDMAHandle.Init.Channel             = SD_DMAx_Tx_CHANNEL;
	xTxDMAHandle.Init.Direction           = DMA_MEMORY_TO_PERIPH;
	xTxDMAHandle.Init.PeriphInc           = DMA_PINC_DISABLE;
	xTxDMAHandle.Init.MemInc              = DMA_MINC_ENABLE;
	xTxDMAHandle.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
	xTxDMAHandle.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
	xTxDMAHandle.Init.Mode                = DMA_PFCTRL;
	xTxDMAHandle.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
	xTxDMAHandle.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
	xTxDMAHandle.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
	xTxDMAHandle.Init.MemBurst            = DMA_MBURST_SINGLE;
	xTxDMAHandle.Init.PeriphBurst         = DMA_PBURST_INC4;

	/* DMA2_Stream6. */
	xTxDMAHandle.Instance = SD_DMAx_Tx_STREAM;

	/* Associate the DMA handle */
	__HAL_LINKDMA(&SDHandle, hdmatx, xTxDMAHandle);

	/* Deinitialize the stream for new transfer */
	HAL_DMA_DeInit(&xTxDMAHandle);

	/* Configure the DMA stream */
	HAL_DMA_Init(&xTxDMAHandle);

	/* NVIC configuration for DMA transfer complete interrupt */
	HAL_NVIC_SetPriority(SD_DMAx_Rx_IRQn, configSDIO_DMA_INTERRUPT_PRIORITY + 2, 0);
	HAL_NVIC_EnableIRQ(SD_DMAx_Rx_IRQn);

	/* NVIC configuration for DMA transfer complete interrupt */
	HAL_NVIC_SetPriority(SD_DMAx_Tx_IRQn, configSDIO_DMA_INTERRUPT_PRIORITY + 2, 0);
	HAL_NVIC_EnableIRQ(SD_DMAx_Tx_IRQn);
}
/*-----------------------------------------------------------*/

void SDIO_IRQHandler(void)
{
	HAL_SD_IRQHandler( &SDHandle );
}

/*-----------------------------------------------------------*/


void DMA2_Stream6_IRQHandler(void)
{
	/* DMA SDIO-TX interrupt handler. */
	HAL_DMA_IRQHandler (SDHandle.hdmatx);
}

/*-----------------------------------------------------------*/


void DMA2_Stream3_IRQHandler(void)
{
	/* DMA SDIO-RX interrupt handler. */
	HAL_DMA_IRQHandler (SDHandle.hdmarx);
}

/*-----------------------------------------------------------*/
