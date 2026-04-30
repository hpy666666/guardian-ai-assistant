/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-03-16     RealThread   first version
 */

#include <rtthread.h>
#include <board.h>
#include <drv_common.h>

#ifndef RT_WEAK
#define RT_WEAK rt_weak
#endif

RT_WEAK void rt_hw_board_init()
{
    extern void hw_board_init(char *clock_src, int32_t clock_src_freq, int32_t clock_target_freq);

    /* Heap initialization */
#if defined(RT_USING_HEAP)
    rt_system_heap_init((void *) HEAP_BEGIN, (void *) HEAP_END);
#endif

    hw_board_init(BSP_CLOCK_SOURCE, BSP_CLOCK_SOURCE_FREQ_MHZ, BSP_CLOCK_SYSTEM_FREQ_MHZ);

    /* Set the shell console output device */
#if defined(RT_USING_DEVICE) && defined(RT_USING_CONSOLE)
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
#endif

    /* Board underlying hardware initialization */
#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif

}

/**
  * @brief ADC MSP Initialization
  * This function configures the hardware resources used in this example
  * @param hadc: ADC handle pointer
  * @retval None
  */
void HAL_ADC_MspInit(ADC_HandleTypeDef* hadc)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    if(hadc->Instance==ADC1)
    {
        /* Peripheral clock enable */
        __HAL_RCC_ADC1_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();

        /**ADC1 GPIO Configuration
        PA4     ------> ADC1_IN4 (MQ-4)
        PA5     ------> ADC1_IN5 (MQ-7)
        */
        GPIO_InitStruct.Pin = GPIO_PIN_4|GPIO_PIN_5;
        GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    }
}

/**
  * @brief ADC MSP De-Initialization
  * This function freeze the hardware resources used in this example
  * @param hadc: ADC handle pointer
  * @retval None
  */
void HAL_ADC_MspDeInit(ADC_HandleTypeDef* hadc)
{
    if(hadc->Instance==ADC1)
    {
        /* Peripheral clock disable */
        __HAL_RCC_ADC1_CLK_DISABLE();

        /**ADC1 GPIO Configuration
        PA4     ------> ADC1_IN4
        PA5     ------> ADC1_IN5
        */
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_4|GPIO_PIN_5);
    }
}

/**
  * @brief SPI MSP Initialization
  * @param hspi: SPI handle pointer
  * @retval None
  * @note  SPI2 was used for external SD card module.
  *        SD card is now handled by onboard SDIO - SPI2 no longer needed.
  */
void HAL_SPI_MspInit(SPI_HandleTypeDef* hspi)
{
    (void)hspi;
}

/**
  * @brief SD card (SDIO) MSP Initialization
  *        Configures SDIO GPIO pins for the onboard SD card slot:
  *        PC8=D0, PC9=D1, PC10=D2, PC11=D3, PC12=CLK, PD2=CMD
  * @param hsd: SD handle pointer
  * @retval None
  */
void HAL_SD_MspInit(SD_HandleTypeDef* hsd)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (hsd->Instance == SDIO)
    {
        /* Enable peripheral clocks */
        __HAL_RCC_SDIO_CLK_ENABLE();
        __HAL_RCC_GPIOC_CLK_ENABLE();
        __HAL_RCC_GPIOD_CLK_ENABLE();

        /**SDIO GPIO Configuration
        PC8  ------> SDIO_D0
        PC9  ------> SDIO_D1
        PC10 ------> SDIO_D2
        PC11 ------> SDIO_D3
        PC12 ------> SDIO_CK
        PD2  ------> SDIO_CMD
        */
        GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
                              GPIO_PIN_11 | GPIO_PIN_12;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_PULLUP;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
        HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

        GPIO_InitStruct.Pin = GPIO_PIN_2;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_PULLUP;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
        HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

        /* SDIO peripheral interrupt */
        HAL_NVIC_SetPriority(SDIO_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(SDIO_IRQn);

        /* DMA2 Stream3 (SDIO RX) interrupt */
        HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 6, 0);
        HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);

        /* DMA2 Stream6 (SDIO TX) interrupt */
        HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 6, 0);
        HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);
    }
}

