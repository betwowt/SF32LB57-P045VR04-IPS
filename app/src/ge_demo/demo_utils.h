/*
 * demo_utils.h - shared math / colour helpers for the GE demo suite.
 * Ported from the ArtInChip GE demos to SF32LB57.
 */
#ifndef _DEMO_UTILS_H_
#define _DEMO_UTILS_H_

#include <rtthread.h>
#include <math.h>

/* --- 系统调试标记 --- */
#define DBG_TAG "GE_DEMO"

/* --- 常用数学帮助宏 --- */
#ifndef ABS
#define ABS(x) ((x) < 0 ? -(x) : (x))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef CLAMP
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#endif

#define PI 3.1415926535f

/* --- 定点数运算帮助宏 (Q12) --- */
#define Q12_SHIFT  12
#define Q12_ONE    (1 << Q12_SHIFT)
#define Q12(f)     ((int)((f) * Q12_ONE))
#define Q12_INT(i) ((i) << Q12_SHIFT)

/* --- 定点数运算帮助宏 (Q8) --- */
#define Q8_SHIFT 8
#define Q8_ONE   (1 << Q8_SHIFT)
#define Q8(f)    ((int)((f) * Q8_ONE))

/* --- 颜色格式转换 (RGB565) --- */
#define RGB2RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3))

#define RGB565_R(c) ((((c) & 0xF800) >> 11) << 3)
#define RGB565_G(c) ((((c) & 0x07E0) >> 5) << 2)
#define RGB565_B(c) (((c) & 0x001F) << 3)

/* --- 内存对齐控制 --- */
#define DEMO_ALIGN_SIZE(x) (((x) + 63) & ~63)

#endif /* _DEMO_UTILS_H_ */
