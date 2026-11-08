/**
  ******************************************************************************
  * @file    py32f0xx_hal_msp.c
  * @brief   HAL MSP: USART1(日志)引脚配置
  * @note    USART1_TX = PA9 (AF1), 只使用发送, 所以不配置RX引脚.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/**
  * @brief  Initializes the Global MSP.
  */
void HAL_MspInit(void)
{
}

/**
  * @brief  Initializes the USART1 MSP.
  */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (huart->Instance == LOG_UART)
  {
    /* Enable USART1 clock and GPIOA clock */
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA9 : USART1_TX */
    GPIO_InitStruct.Pin       = LOG_TX_PIN;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_PULLUP;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = LOG_TX_AF;
    HAL_GPIO_Init(LOG_UART_PORT, &GPIO_InitStruct);
  }
}

/**
  * @brief  DeInitializes the USART1 MSP.
  */
void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
  if (huart->Instance == LOG_UART)
  {
    __HAL_RCC_USART1_FORCE_RESET();
    __HAL_RCC_USART1_RELEASE_RESET();
    HAL_GPIO_DeInit(LOG_UART_PORT, LOG_TX_PIN);
  }
}
