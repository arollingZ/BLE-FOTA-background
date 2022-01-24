#ifndef __DFU_BLE_H_
#define __DFU_BLE_H_


extern void dfu_ble_write_handler(uint8_t *p_ble_evt,uint32_t size);
extern void dfu_ble_authorize_handler(uint8_t *p_ble_evt,uint32_t size);
#endif
