#ifndef __DFU_INIT_H_
#define __DFU_INIT_H_

#include "nrf_dfu.h"
#include "nrf_dfu_transport.h"
#include "nrf_dfu_types.h"
#include "nrf_dfu_req_handler.h"
#include "nrf_dfu_handling_error.h"
#include "nrf_sdm.h"
#include "nrf_dfu_mbr.h"
#include "nrf_bootloader_info.h"
#include "nrf_balloc.h"
#include "nrf_dfu_settings.h"
#include "nrf_dfu_ble.h"

extern void dfu_service_init(void);

#endif

