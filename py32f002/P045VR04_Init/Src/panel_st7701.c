/**
  ******************************************************************************
  * @file    panel_st7701.c
  * @brief   P045VR04-IPS(480x854, ST7701S)面板初始化
  * @note    SPI时序与初始化表完全对应ESP32参考例程:
  *          ESP32-LCD/IDF/P045IVR04-IPS-CTP/main/Src/lcd.c (lcd_init_cmds[])
  *          以及 esp_lcd_panel_io_3wire_spi / esp_lcd_st7701 两个组件的时序.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/*---------------------------------------------------------------------------*/
/* 3线SPI(9bit)软件时序说明:                                                 */
/*   SCL空闲为低, SDA在SCL为低时变化, 上升沿被面板采样(SPI mode0);           */
/*   每个字节先发送1bit DC位: 命令=0, 数据=1, 然后发送8bit数据(MSB first);  */
/*   每个字节单独产生一次CS有效脉冲(与参考实现相同).                         */
/*   对应配置: spi_mode=0, use_dc_bit=1, dc_zero_on_data=0, lsb_first=0.     */
/*---------------------------------------------------------------------------*/

/* Private types -------------------------------------------------------------*/
typedef struct
{
  uint8_t         cmd;        /* 命令 */
  uint8_t         len;        /* 参数个数 */
  const uint8_t  *data;       /* 参数 */
  uint16_t        delay_ms;   /* 发送后延时(ms) */
} panel_init_cmd_t;

/* Private function prototypes -----------------------------------------------*/
static void st7701_spi_delay(void);
static void st7701_spi_byte(uint8_t dc, uint8_t data);
static void st7701_spi_cmd(uint8_t cmd, const uint8_t *data, uint8_t len);

/*---------------------------------------------------------------------------*/
/* 发送初始化表之前的固定命令(与esp_lcd_st7701驱动 panel_st7701_send_init_cmds */
/* 一致): 先关闭Command2(BKxSEL), 再设置MADCTL和COLMOD.                       */
/*---------------------------------------------------------------------------*/
static const uint8_t st7701_bkxsel_dis[5] = {0x77, 0x01, 0x00, 0x00, 0x00};   /* Command2关闭 */
static const uint8_t st7701_madctl[1]     = {0x00};                           /* MADCTL: RGB顺序,不镜像 */
static const uint8_t st7701_colmod[1]     = {0x50};                           /* COLMOD: 16bit(RGB565)接口格式,
                                                                               详见下方说明 */

/*---------------------------------------------------------------------------*/
/* ST7701S 初始化表: 命令|参数个数|参数|发送后延时(ms)                        */
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
/* ST7701S 初始化表                                                    */
/*   来源: 屏厂 "ST7701S_ IVO4.5IPS(C045SWY2-0)_initialcode_V1.1"       */
/*   格式: 命令|参数个数|参数|发送后延时(ms)                            */
/*   与通用版(4.5IVO+ST7701S RGB, 即网上多数ST7701S代码)的主要差异:     */
/*     1) 开头多一条 0xFF(0x13页) + 0xEF=0x08;                         */
/*     2) BK0 0xB0/0xB1 两条16字节伽马表值完全不同(决定灰阶/白平衡);   */
/*     3) BK1 0xB1=0x2A/0xB2=0x87(通用版为0x1C/0x07, 与VCOM有关);      */
/*     4) 新增 BK0 0xEF(6字节);                                        */
/*     5) 电源段(D0)之后有 100ms 稳定延时;                             */
/*     6) Sleep Out(0x11)移到全部配置之后, 且新增 0x13页 0xE8 时序处理. */
/*---------------------------------------------------------------------------*/
static const uint8_t st7701_p1[5] = {0x77, 0x01, 0x00, 0x00, 0x13};
static const uint8_t st7701_p2[1] = {0x08};
static const uint8_t st7701_p3[5] = {0x77, 0x01, 0x00, 0x00, 0x10};
static const uint8_t st7701_p4[2] = {0xE9, 0x03};
static const uint8_t st7701_p5[2] = {0x12, 0x02};
static const uint8_t st7701_p6[2] = {0x31, 0x08};
static const uint8_t st7701_p7[1] = {0x10};
static const uint8_t st7701_p8[16] = {0x05, 0x0F, 0x14, 0x0E, 0x12, 0x08, 0x05, 0x09, 0x08, 0x22, 0x07, 0x16, 0x14, 0x25, 0x28, 0x09};
static const uint8_t st7701_p9[16] = {0x05, 0x0D, 0x14, 0x0D, 0x10, 0x06, 0x02, 0x07, 0x07, 0x19, 0x05, 0x12, 0x10, 0x20, 0x24, 0x09};
static const uint8_t st7701_p10[5] = {0x77, 0x01, 0x00, 0x00, 0x11};
static const uint8_t st7701_p11[1] = {0x4D};
static const uint8_t st7701_p12[1] = {0x2A};
static const uint8_t st7701_p13[1] = {0x87};
static const uint8_t st7701_p14[1] = {0x80};
static const uint8_t st7701_p15[1] = {0x47};
static const uint8_t st7701_p16[1] = {0x85};
static const uint8_t st7701_p17[1] = {0x21};
static const uint8_t st7701_p18[1] = {0x10};
static const uint8_t st7701_p19[1] = {0x78};
static const uint8_t st7701_p20[1] = {0x78};
static const uint8_t st7701_p21[1] = {0x88};
static const uint8_t st7701_p22[3] = {0x00, 0x00, 0x02};
static const uint8_t st7701_p23[11] = {0x0B, 0x00, 0x0D, 0x00, 0x0C, 0x00, 0x0E, 0x00, 0x00, 0x44, 0x44};
static const uint8_t st7701_p24[13] = {0x33, 0x33, 0x44, 0x44, 0x64, 0x00, 0x66, 0x00, 0x65, 0x00, 0x67, 0x00, 0x00};
static const uint8_t st7701_p25[4] = {0x00, 0x00, 0x33, 0x33};
static const uint8_t st7701_p26[2] = {0x44, 0x44};
static const uint8_t st7701_p27[16] = {0x0C, 0x78, 0xA0, 0xA0, 0x0E, 0x78, 0xA0, 0xA0, 0x10, 0x78, 0xA0, 0xA0, 0x12, 0x78, 0xA0, 0xA0};
static const uint8_t st7701_p28[4] = {0x00, 0x00, 0x33, 0x33};
static const uint8_t st7701_p29[2] = {0x44, 0x44};
static const uint8_t st7701_p30[16] = {0x0D, 0x78, 0xA0, 0xA0, 0x0F, 0x78, 0xA0, 0xA0, 0x11, 0x78, 0xA0, 0xA0, 0x13, 0x78, 0xA0, 0xA0};
static const uint8_t st7701_p31[7] = {0x02, 0x00, 0x39, 0x39, 0xEE, 0x44, 0x00};
static const uint8_t st7701_p32[2] = {0x00, 0x00};
static const uint8_t st7701_p33[16] = {0xFF, 0xF1, 0x04, 0x56, 0x72, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF3, 0x27, 0x65, 0x40, 0x1F, 0xFF};
static const uint8_t st7701_p34[6] = {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F};
static const uint8_t st7701_p35[5] = {0x77, 0x01, 0x00, 0x00, 0x13};
static const uint8_t st7701_p36[2] = {0x00, 0x0E};
static const uint8_t st7701_p37[5] = {0x77, 0x01, 0x00, 0x00, 0x00};
static const uint8_t st7701_p39[5] = {0x77, 0x01, 0x00, 0x00, 0x13};
static const uint8_t st7701_p40[2] = {0x00, 0x0C};
static const uint8_t st7701_p41[2] = {0x00, 0x00};
static const uint8_t st7701_p42[5] = {0x77, 0x01, 0x00, 0x00, 0x00};

static const panel_init_cmd_t st7701_init_tbl[] =
{
    {0xFF, 5, st7701_p1, 0},
    {0xEF, 1, st7701_p2, 0},
    {0xFF, 5, st7701_p3, 0},
    {0xC0, 2, st7701_p4, 0},
    {0xC1, 2, st7701_p5, 0},
    {0xC2, 2, st7701_p6, 0},
    {0xCC, 1, st7701_p7, 0},
    {0xB0, 16, st7701_p8, 0},
    {0xB1, 16, st7701_p9, 0},
    {0xFF, 5, st7701_p10, 0},
    {0xB0, 1, st7701_p11, 0},
    {0xB1, 1, st7701_p12, 0},
    {0xB2, 1, st7701_p13, 0},
    {0xB3, 1, st7701_p14, 0},
    {0xB5, 1, st7701_p15, 0},
    {0xB7, 1, st7701_p16, 0},
    {0xB8, 1, st7701_p17, 0},
    {0xB9, 1, st7701_p18, 0},
    {0xC1, 1, st7701_p19, 0},
    {0xC2, 1, st7701_p20, 0},
    {0xD0, 1, st7701_p21, 100},
    {0xE0, 3, st7701_p22, 0},
    {0xE1, 11, st7701_p23, 0},
    {0xE2, 13, st7701_p24, 0},
    {0xE3, 4, st7701_p25, 0},
    {0xE4, 2, st7701_p26, 0},
    {0xE5, 16, st7701_p27, 0},
    {0xE6, 4, st7701_p28, 0},
    {0xE7, 2, st7701_p29, 0},
    {0xE8, 16, st7701_p30, 0},
    {0xEB, 7, st7701_p31, 0},
    {0xEC, 2, st7701_p32, 0},
    {0xED, 16, st7701_p33, 0},
    {0xEF, 6, st7701_p34, 0},
    {0xFF, 5, st7701_p35, 0},
    {0xE8, 2, st7701_p36, 0},
    {0xFF, 5, st7701_p37, 0},
    {0x11, 0, NULL, 120},                                              /* Sleep Out */
    {0xFF, 5, st7701_p39, 0},
    {0xE8, 2, st7701_p40, 10},
    {0xE8, 2, st7701_p41, 0},
    {0xFF, 5, st7701_p42, 0},
    {0x29, 0, NULL, 20},                                               /* Display ON */
};

/**
  * @brief  3线SPI半周期延时(约1us, 对应SCL约500kHz, 与参考实现相同)
  * @note   24MHz主频, 每次循环约4个周期; 修改PANEL_SPI_HALF_LOOPS可调整速度
  * @retval None
  */
static void st7701_spi_delay(void)
{
  volatile uint8_t i = PANEL_SPI_HALF_LOOPS;

  while (i--)
  {
  }
}

/**
  * @brief  发送1个字节(9bit: DC位 + 8bit数据, MSB first)
  * @param  dc   : 0,命令; 1,数据
  * @param  data : 要发送的字节
  * @retval None
  */
static void st7701_spi_byte(uint8_t dc, uint8_t data)
{
  uint16_t frame = (uint16_t)(((uint16_t)(dc & 0x01) << 8) | data);
  uint8_t i;

  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_CS_PIN, GPIO_PIN_RESET);      /* CS有效 */
  st7701_spi_delay();

  for (i = 0; i < 9; i++)
  {
    /* SDA在SCL为低时变化 */
    if (frame & 0x0100)
    {
      HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_SDA_PIN, GPIO_PIN_SET);
    }
    else
    {
      HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_SDA_PIN, GPIO_PIN_RESET);
    }

    frame <<= 1;                                                      /* MSB first */
    st7701_spi_delay();
    HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_SCK_PIN, GPIO_PIN_SET);      /* 上升沿采样 */
    st7701_spi_delay();
    HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_SCK_PIN, GPIO_PIN_RESET);    /* 时钟回到空闲低 */
  }

  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_SDA_PIN, GPIO_PIN_RESET);      /* SDA回到空闲低 */
  st7701_spi_delay();
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_CS_PIN, GPIO_PIN_SET);         /* CS结束 */
  st7701_spi_delay();
}

/**
  * @brief  发送1条命令+参数
  * @param  cmd  : 命令
  * @param  data : 参数缓冲区(可以为NULL)
  * @param  len  : 参数个数
  * @retval None
  */
static void st7701_spi_cmd(uint8_t cmd, const uint8_t *data, uint8_t len)
{
  st7701_spi_byte(0, cmd);                                             /* DC=0: 命令 */

  while (len--)
  {
    st7701_spi_byte(1, *data++);                                       /* DC=1: 数据 */
  }
}

/**
  * @brief  面板相关IO初始化(READY=0, CS=1, SCL/SDA=0, RST=0)
  * @param  None
  * @retval None
  */
void panel_gpio_init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();

  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Pin   = LCD_SCK_PIN | LCD_SDA_PIN | LCD_CS_PIN | LCD_RST_PIN | LCD_READY_PIN;
  HAL_GPIO_Init(LCD_PANEL_PORT, &GPIO_InitStruct);

  /* 1. READY=0; CS=1; SCL/SDA为3线SPI空闲电平(低) */
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_READY_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_CS_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_SCK_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_SDA_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_RST_PIN, GPIO_PIN_RESET);
}

/**
  * @brief  面板硬件复位(与esp_lcd_st7701的reset时序一致)
  * @param  None
  * @retval None
  */
void panel_st7701_reset(void)
{
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_RST_PIN, GPIO_PIN_RESET);     /* 复位有效 */
  HAL_Delay(PANEL_RST_LOW_MS);
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_RST_PIN, GPIO_PIN_SET);       /* 释放复位 */
  HAL_Delay(PANEL_RST_HIGH_MS);

  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_CS_PIN, GPIO_PIN_SET);        /* CS无效 */
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_SCK_PIN, GPIO_PIN_RESET);     /* SCL空闲低 */
  HAL_GPIO_WritePin(LCD_PANEL_PORT, LCD_SDA_PIN, GPIO_PIN_RESET);     /* SDA空闲低 */
}

/**
  * @brief  面板初始化: 等待上电 -> 复位 -> 发送初始化表
  * @note   本函数返回后即可置READY=1(内部已包含表格要求的所有延时)
  * @param  None
  * @retval None
  */
void panel_st7701_init(void)
{
  uint32_t i;

  HAL_Delay(PANEL_POWER_ON_MS);                                       /* 2. 等待电源稳定 */

  panel_st7701_reset();                                               /*    复位面板 */

  /* 说明: COLMOD=0x50 为16bit(RGB565)接口(VIPF=0101), 面板从每通道的**低位脚**取数:
   * R4..R0=DB20..16, G5..G0=DB13..08, B4..B0=DB04..00.
   * 因此转接板必须把H743 LTDC输出的每通道高位(R3..R7 / G2..G7 / B3..B7)
   * 对应接到面板的 R0..R4 / G0..G5 / B0..B4(即"标准RGB565"接线).
   * 若仍使用按"高位对高位"接线的 REV 0.1-DRAFT 转接板(面板低位接地),
   * 则必须改回24bit(RGB888)模式 COLMOD=0x70(VIPF=0111), 否则颜色会错乱.
   */
  /* 发送初始化表之前的固定命令 */
  st7701_spi_cmd(0xFF, st7701_bkxsel_dis, 5);                         /* 关闭Command2 */
  st7701_spi_cmd(0x36, st7701_madctl, 1);                             /* MADCTL */
  st7701_spi_cmd(0x3A, st7701_colmod, 1);                             /* COLMOD(16bit/RGB565) */

  /* 3. 发送面板初始化表(含所有必需延时) */
  for (i = 0; i < (sizeof(st7701_init_tbl) / sizeof(st7701_init_tbl[0])); i++)
  {
    st7701_spi_cmd(st7701_init_tbl[i].cmd, st7701_init_tbl[i].data, st7701_init_tbl[i].len);

    if (st7701_init_tbl[i].delay_ms != 0)
    {
      HAL_Delay(st7701_init_tbl[i].delay_ms);
    }
  }
}
