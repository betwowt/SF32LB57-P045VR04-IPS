/*
 * Minimal ArtInChip core shim used by the effect sources.
 */
#ifndef _AIC_CORE_SHIM_H_
#define _AIC_CORE_SHIM_H_

#include <rtthread.h>
#include <stddef.h>
#include <stdint.h>

#include "bf0_hal.h"
#include "system_bf0_ap.h"

#define LOG_E(fmt, ...) rt_kprintf("[GE][E] " fmt "\n", ##__VA_ARGS__)
#define LOG_W(fmt, ...) rt_kprintf("[GE][W] " fmt "\n", ##__VA_ARGS__)
#define LOG_I(fmt, ...) rt_kprintf("[GE] " fmt "\n", ##__VA_ARGS__)

/* Both helpers accept unaligned ranges and align them to the 32-byte cache
 * line before touching the cache maintenance registers. */
static inline void aicos_dcache_clean_range(void *addr, size_t size)
{
    uintptr_t start = ((uintptr_t)addr) & ~(uintptr_t)31;
    uintptr_t end   = (((uintptr_t)addr) + size + 31) & ~(uintptr_t)31;

    if (end > start)
        mpu_dcache_clean((void *)start, (uint32_t)(end - start));
}

static inline void aicos_dcache_invalid_range(void *addr, size_t size)
{
    uintptr_t start = ((uintptr_t)addr) & ~(uintptr_t)31;
    uintptr_t end   = (((uintptr_t)addr) + size + 31) & ~(uintptr_t)31;

    if (end > start)
        mpu_dcache_invalidate((void *)start, (uint32_t)(end - start));
}

#endif /* _AIC_CORE_SHIM_H_ */
