/*
 * Wi-Fi provisioning (SoftAP + captive page) for the xiaozhi port.
 *
 * Flow: no stored credentials -> start AP "Xiaozhi-XXXX" -> the page at
 * http://192.168.4.1 lists visible SSIDs and accepts an SSID/password ->
 * credentials are written to flash -> reboot into station mode.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _XZ_PROVISION_H_
#define _XZ_PROVISION_H_

#include <stdbool.h>
#include <stddef.h>

#define XZ_PROV_AP_PREFIX   "Xiaozhi-"
#define XZ_PROV_AP_PASS     "12345678"
#define XZ_PROV_URL         "http://192.168.4.1"

bool xz_prov_load(char *ssid, size_t ssid_sz, char *pwd, size_t pwd_sz);
bool xz_prov_save(const char *ssid, const char *pwd);
void xz_prov_erase(void);

/* Bring the chip up, start the SoftAP and serve the config page.
 * Does not return on success: reboots the board after saving credentials. */
void xz_prov_start(void);

const char *xz_prov_ap_ssid(void);

#endif /* _XZ_PROVISION_H_ */
