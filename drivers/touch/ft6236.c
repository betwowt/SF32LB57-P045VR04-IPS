/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FocalTech FT6236 / FT6x36 compatible capacitive touch driver.
 * Address 0x38, 10-bit X/Y, panel native orientation 480x854.
 */

#include <rtthread.h>
#include "board.h"
#include "ft6236.h"
#include "drv_touch.h"

#define DBG_LEVEL          DBG_ERROR
#define LOG_TAG            "drv.ft6236"
#include <drv_log.h>

#define FT_DEV_ADDR         0x38
#define FT_TD_STATUS        0x02
#define FT_P1_XH            0x03
#define FT_P1_XL            0x04
#define FT_P1_YH            0x05
#define FT_P1_YL            0x06
#define FT_ID_G_MODE        0xA4

#define FT_MAX_WIDTH        480
#define FT_MAX_HEIGHT       854

static rt_err_t write_reg(uint8_t reg, uint8_t data);
static rt_err_t read_regs(uint8_t reg, uint8_t len, uint8_t *buf);

static struct rt_i2c_bus_device *ft_bus = NULL;
static struct touch_drivers ft6236_driver;

static rt_err_t write_reg(uint8_t reg, uint8_t data)
{
    struct rt_i2c_msg msgs;
    uint8_t buf[2] = {reg, data};

    msgs.addr  = FT_DEV_ADDR;
    msgs.flags = RT_I2C_WR;
    msgs.buf   = buf;
    msgs.len   = 2;

    return (rt_i2c_transfer(ft_bus, &msgs, 1) == 1) ? RT_EOK : -RT_ERROR;
}

static rt_err_t read_regs(uint8_t reg, uint8_t len, uint8_t *buf)
{
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = FT_DEV_ADDR;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = &reg;
    msgs[0].len   = 1;

    msgs[1].addr  = FT_DEV_ADDR;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = buf;
    msgs[1].len   = len;

    return (rt_i2c_transfer(ft_bus, msgs, 2) == 2) ? RT_EOK : -RT_ERROR;
}

static void ft6236_correct_pos(touch_msg_t ppos)
{
    if (ppos->x >= FT_MAX_WIDTH)
    {
        ppos->x = FT_MAX_WIDTH - 1;
    }
    if (ppos->y >= FT_MAX_HEIGHT)
    {
        ppos->y = FT_MAX_HEIGHT - 1;
    }
}

static rt_err_t read_point(touch_msg_t p_msg)
{
    uint8_t buf[4];
    uint8_t tp_num;
    rt_err_t err;

    rt_touch_irq_pin_enable(1);

    if (!ft_bus || !p_msg)
    {
        return RT_ERROR;
    }

    err = read_regs(FT_TD_STATUS, 1, &tp_num);
    if (RT_EOK != err)
    {
        goto error;
    }

    tp_num &= 0x0F;
    if (tp_num > 0)
    {
        err = read_regs(FT_P1_XH, sizeof(buf), buf);
        if (RT_EOK != err)
        {
            goto error;
        }

        p_msg->x = (((uint16_t)buf[0] & 0x0F) << 8) | buf[1];
        p_msg->y = (((uint16_t)buf[2] & 0x0F) << 8) | buf[3];
        ft6236_correct_pos(p_msg);

        switch (buf[0] >> 6)
        {
        case 0: /* put down */
        case 2: /* contact */
            p_msg->event = TOUCH_EVENT_DOWN;
            break;
        case 1: /* put up */
            p_msg->event = TOUCH_EVENT_UP;
            break;
        default:
            p_msg->event = TOUCH_EVENT_NONE;
            break;
        }

        return (tp_num > 1) ? RT_EOK : RT_EEMPTY;
    }

    p_msg->x = 0;
    p_msg->y = 0;
    p_msg->event = TOUCH_EVENT_UP;
    return RT_EEMPTY;

error:
    p_msg->x = 0;
    p_msg->y = 0;
    p_msg->event = TOUCH_EVENT_UP;
    return RT_ERROR;
}

static void ft6236_irq_handler(void *arg)
{
    rt_touch_irq_pin_enable(0);
    rt_sem_release(ft6236_driver.isr_sem);
}

static rt_err_t init(void)
{
    rt_touch_irq_pin_attach(PIN_IRQ_MODE_FALLING, ft6236_irq_handler, NULL);
    rt_touch_irq_pin_enable(1);

    if (RT_EOK != write_reg(FT_ID_G_MODE, 1))
    {
        LOG_E("G_MODE set fail");
    }

    return RT_EOK;
}

static rt_err_t deinit(void)
{
    rt_touch_irq_pin_enable(0);
    return RT_EOK;
}

static rt_bool_t probe(void)
{
    ft_bus = (struct rt_i2c_bus_device *)rt_device_find(TOUCH_DEVICE_NAME);
    if (!ft_bus || ft_bus->parent.type != RT_Device_Class_I2CBUS)
    {
        ft_bus = NULL;
        LOG_E("i2c bus %s not found", TOUCH_DEVICE_NAME);
        return RT_FALSE;
    }

    rt_device_open((rt_device_t)ft_bus,
                   RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_TX | RT_DEVICE_FLAG_INT_RX);

    struct rt_i2c_configuration configuration =
    {
        .mode = 0,
        .addr = 0,
        .timeout = 500,
        .max_hz = 400000,
    };
    rt_i2c_configure(ft_bus, &configuration);

    LOG_I("ft6236 probe OK");
    return RT_TRUE;
}

static struct touch_ops ops =
{
    read_point,
    init,
    deinit
};

static int rt_ft6236_init(void)
{
    ft6236_driver.probe = probe;
    ft6236_driver.ops = &ops;
    ft6236_driver.user_data = RT_NULL;
    ft6236_driver.isr_sem = rt_sem_create("ft6236", 0, RT_IPC_FLAG_FIFO);

    rt_touch_drivers_register(&ft6236_driver);

    return 0;
}
INIT_COMPONENT_EXPORT(rt_ft6236_init);
