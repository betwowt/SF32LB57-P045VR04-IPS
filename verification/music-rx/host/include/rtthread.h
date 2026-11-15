#ifndef TEST_RTTHREAD_H
#define TEST_RTTHREAD_H
#include <stdint.h>
#include <stddef.h>
typedef size_t rt_size_t;
typedef unsigned long rt_tick_t;
typedef long rt_base_t;
typedef void *rt_device_t;
struct rt_ringbuffer32;
#define RT_NULL NULL
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EBUSY 7
rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t level);
rt_tick_t rt_tick_get(void);
void rt_kprintf(const char *format, ...);
#endif
