#include "board.h"
#include "dfu_init.h"
#include "dfu_ble.h"
#include "ble_service_init.h"


static void bootloader_reset(void)
{
    port_trace("Download firmware success!Resetting application.\n");
    NVIC_SystemReset();
}
/**@brief Function for handling DFU events.
 */
static void dfu_observer(nrf_dfu_evt_type_t evt_type)
{
    switch (evt_type) {
        case NRF_DFU_EVT_DFU_STARTED:
        case NRF_DFU_EVT_OBJECT_RECEIVED:

            break;
        case NRF_DFU_EVT_DFU_COMPLETED:
            bootloader_reset();
            break;
        case NRF_DFU_EVT_DFU_ABORTED:
            nrf_dfu_settings_init(true);
            break;
        case NRF_DFU_EVT_TRANSPORT_DEACTIVATED:
            // Reset the internal state of the DFU settings to the last stored state.
            nrf_dfu_settings_reinit();
            break;
        default:
            break;
    }

}
/*
1.初始化settings
2.创建DFU服务
*/
static void dfu_init(void)
{
    uint32_t ret_val;

    ret_val = nrf_dfu_settings_init(true);
    APP_ERROR_CHECK(ret_val);

    ret_val = nrf_dfu_init(dfu_observer);
    APP_ERROR_CHECK(ret_val);
}

void dfu_service_init(void)
{
    port_trace("dfu service init! \n");
    dfu_init();
    //user_advertising_start();
}


