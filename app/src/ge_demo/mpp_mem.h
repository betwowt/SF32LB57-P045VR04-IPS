#ifndef _MPP_MEM_SHIM_H_
#define _MPP_MEM_SHIM_H_

#include <stddef.h>

/* Buffers handed to EPIC live in PSRAM: physically contiguous, cacheable,
 * 64-byte aligned.  mpp_alloc() serves the per-effect lookup tables too. */
unsigned int mpp_phy_alloc(size_t size);
void         mpp_phy_free(unsigned int addr);
void        *mpp_alloc(size_t size);
void         mpp_free(void *ptr);

#endif /* _MPP_MEM_SHIM_H_ */
