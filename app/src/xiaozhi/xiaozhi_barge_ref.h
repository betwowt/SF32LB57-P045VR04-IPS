#ifndef XIAOZHI_BARGE_REF_H
#define XIAOZHI_BARGE_REF_H

#include <stdint.h>

typedef struct
{
    uint16_t rms, peak;
    uint32_t age_ms;
    int32_t skew;
    uint8_t synced;
} xz_barge_ref_t;

/* Called with the same critical section as the detector lifecycle. */
void xz_barge_ref_reset(void);
void xz_barge_ref_snapshot(xz_barge_ref_t *ref);

#endif
