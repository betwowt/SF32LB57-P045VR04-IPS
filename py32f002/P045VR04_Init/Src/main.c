/**
  ******************************************************************************
  * @file    main.c
  * @brief   P045VR04-IPS 屏幕初始化(PY32F002AW15S6TU)
  * @note    H743_RGB_Adapter 转接板上, H743只负责RGB时序, 屏幕的SPI初始化由本芯片完成:
  *          上电 -> READY=0 -> 复位面板 -> 3线SPI发送ST7701S初始化表 -> READY=1
  *          H743(PG3)检测到READY=1后才会打开背光.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private variables ---------------------------------------------------------*/
static UART_HandleTypeDef UartHandle;

/* Private function prototypes -----------------------------------------------*/
static void APP_SystemClockConfig(void);
static void APP_LogInit(void);

/**
  * @brief  Main program.
  * @retval int
  */
int main(void)
{
  /* Reset of all peripherals, Initializes the Systick. */
  HAL_Init();

  /* 第一时间把READY拉低(并设置3线SPI总线空闲电平), 防止H743误判屏幕已初始化 */
  panel_gpio_init();

  /* Configure the system clock: HSI 24MHz */
  APP_SystemClockConfig();

  /* 调试日志串口(PA9) */
  APP_LogInit();

  app_log("\r\n[PY32] P045VR04-IPS init start (480x854, ST7701S, 3-wire SPI)\r\n");

  /* 复位面板并发送初始化表(内部包含全部必需延时) */
  panel_st7701_init();

  /* 所有延时结束, 通知H743(PG3)可以打开背光 */
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_READY_PIN, GPIO_PIN_SET);
  app_log("[PY32] panel init done, READY=1\r\n");

  /* Infinite loop */
  while (1)
  {
  }
}

/**
  * @brief  System Clock Configuration: HSI 24MHz
  * @param  None
  * @retval None
  */
static void APP_SystemClockConfig(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /* Oscillator Configuration */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
#if defined(RCC_HSIDIV_SUPPORT)
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
#endif
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_24MHz;   /* HSI 24MHz */

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    APP_ErrorHandler();
  }

  /* Clock source configuration */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSISYS;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    APP_ErrorHandler();
  }
}

/**
  * @brief  日志串口初始化: USART1_TX(PA9), 115200-8-N-1, 仅发送
  * @param  None
  * @retval None
  */
static void APP_LogInit(void)
{
  UartHandle.Instance        = LOG_UART;
  UartHandle.Init.BaudRate   = 115200;
  UartHandle.Init.WordLength = UART_WORDLENGTH_8B;
  UartHandle.Init.StopBits   = UART_STOPBITS_1;
  UartHandle.Init.Parity     = UART_PARITY_NONE;
  UartHandle.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
  UartHandle.Init.Mode       = UART_MODE_TX;
  UartHandle.Init.OverSampling = UART_OVERSAMPLING_16;
  UartHandle.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

  if (HAL_UART_Init(&UartHandle) != HAL_OK)
  {
    APP_ErrorHandler();
  }
}

/**
  * @brief  输出调试日志(阻塞发送, 超时100ms)
  * @param  str : 以'\0'结束的字符串
  * @retval None
  */
void app_log(const char *str)
{
  uint32_t len = 0;

  while (str[len] != '\0')
  {
    len++;
  }

  if (len == 0)
  {
    return;
  }

  HAL_UART_Transmit(&UartHandle, (uint8_t *)str, (uint16_t)len, 100);
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @param  None
  * @retval None
  */
void APP_ErrorHandler(void)
{
  /* Infinite loop */
  while (1)
  {
  }
}
