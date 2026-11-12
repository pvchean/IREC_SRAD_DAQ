/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t mtp_dispatch(tud_mtp_cb_data_t *cb_data);

/* FORK (MTP-M): link anchor. tinyusb_mtp_transport.c holds the strong tud_mtp_*_cb functions that override WEAK defaults
 * (they return -1) in tinyusb/src/class/mtp/mtp_device.c. Nothing else references that object, so the linker did not pull it
 * from the archive and every MTP command failed. tinyusb_mtp_install_driver() calls this function: the object is pulled. */
void mtp_transport_link_anchor(void);
void mtp_trace_request_result(const tud_mtp_cb_data_t *cb_data, int32_t response);

#ifdef __cplusplus
}
#endif
