/*
 * The SF32 image layout always carries three XIP segments: ER_IROM1 (main),
 * ER_IROM2 (ROM2, previously the ezip emoji payload) and ER_IROM3 (ROM3,
 * previously the TTF font). The face UI needs neither payload, but the ftab
 * generator rejects a zero-sized image, so keep one small, useful blob in
 * each segment. They are printed at boot as a build banner.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

__attribute__((section(".ROM3_IMG"), used, retain))
const char xz_rom2_banner[] =
    "xiaozhi-sf32lb57 rom2 " __DATE__ " " __TIME__;

__attribute__((section(".font_data"), used, retain))
const char xz_rom3_banner[] =
    "xiaozhi-sf32lb57 rom3 " __DATE__ " " __TIME__;
