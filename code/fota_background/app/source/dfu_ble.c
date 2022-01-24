#include "board.h"
#include "nrf_dfu_ble.h"

#include <stddef.h>
#include "sdk_common.h"
#include "nrf_dfu_transport.h"
#include "nrf_dfu_types.h"
#include "nrf_dfu_req_handler.h"
#include "nrf_dfu_handling_error.h"
#include "nrf_sdm.h"
#include "nrf_dfu_mbr.h"
#include "nrf_bootloader_info.h"
#include "ble.h"
#include "ble_srv_common.h"
#include "ble_hci.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_balloc.h"
#include "nrf_delay.h"
#include "nrf_dfu_settings.h"
#include "nrf_dfu_ble.h"
#include "dfu_ble.h"
#include "ble_task.h"

#define GATT_HEADER_LEN                     3                                                       /**< GATT header length. */
#define GATT_PAYLOAD(mtu)                   ((mtu) - GATT_HEADER_LEN)                               /**< Length of the ATT payload for a given ATT MTU. */
#define MAX_DFU_PKT_LEN                     (NRF_SDH_BLE_GATT_MAX_MTU_SIZE - GATT_HEADER_LEN)       /**< Maximum length (in bytes) of the DFU Packet characteristic (3 bytes are used for the GATT opcode and handle). */
#define MAX_RESPONSE_LEN                    17                                                      /**< Maximum length (in bytes) of the response to a Control Point command. */
#define RESPONSE_HEADER_LEN                 3                                                       /**< The length of the header of a response. I.E. the index of the opcode-specific payload. */

#define DFU_BLE_FLAG_INITIALIZED            (1 << 0)                                                /**< Flag to check if the DFU service was initialized by the application.*/
#define DFU_BLE_FLAG_USE_ADV_NAME           (1 << 1)                                                /**< Flag to indicate that advertisement name is to be used. */
#define DFU_BLE_RESETTING_SOON              (1 << 2)                                                /**< Flag to indicate that the device will reset soon. */

#if (NRF_DFU_BLE_BUFFERS_OVERRIDE)
/* If selected, use the override value. */
#define MAX_DFU_BUFFERS     NRF_DFU_BLE_BUFFERS
#else
#define MAX_DFU_BUFFERS     ((CODE_PAGE_SIZE / MAX_DFU_PKT_LEN) + 1)
#endif

DFU_TRANSPORT_REGISTER(nrf_dfu_transport_t const ble_dfu_transport) = {
    .init_func  = ble_dfu_transport_init,
    .close_func = ble_dfu_transport_close,
};


static uint32_t           m_flags;
static ble_dfu_t          m_dfu;                                                                    /**< Structure used to identify the Device Firmware Update service. */
static uint16_t           m_pkt_notif_target;                                                       /**< Number of packets of firmware data to be received before transmitting the next Packet Receipt Notification to the DFU Controller. */
static uint16_t           m_pkt_notif_target_cnt;                                                   /**< Number of packets of firmware data received after sending last Packet Receipt Notification or since the receipt of a @ref BLE_DFU_PKT_RCPT_NOTIF_ENABLED event from the DFU service, which ever occurs later.*/
//static uint16_t           m_conn_handle = BLE_CONN_HANDLE_INVALID;                                  /**< Handle of the current connection. */
extern uint16_t           m_conn_handle;

NRF_BALLOC_DEF(m_buffer_pool, MAX_DFU_PKT_LEN, MAX_DFU_BUFFERS);


static bool is_cccd_configured(ble_dfu_t *p_dfu)
{
    uint8_t cccd_val_buf[BLE_CCCD_VALUE_LEN];

    ble_gatts_value_t gatts_value = {
        .len     = BLE_CCCD_VALUE_LEN,
        .p_value = cccd_val_buf
    };

    /* Check the CCCD Value of DFU Control Point. */
    uint32_t err_code = sd_ble_gatts_value_get(m_conn_handle,
                        p_dfu->dfu_ctrl_pt_handles.cccd_handle,
                        &gatts_value);
    VERIFY_SUCCESS(err_code);

    return ble_srv_is_notification_enabled(cccd_val_buf);
}


static ret_code_t response_send(uint8_t *p_buf, uint16_t len)
{
    ble_gatts_hvx_params_t hvx_params = {
        .handle = m_dfu.dfu_ctrl_pt_handles.value_handle,
        .type   = BLE_GATT_HVX_NOTIFICATION,
        .p_data = (uint8_t *)(p_buf),
        .p_len  = &len,
    };

    return sd_ble_gatts_hvx(m_conn_handle, &hvx_params);
}


/**@brief Function for encoding the beginning of a response.
 *
 * @param[inout] p_buffer  The buffer to encode into.
 * @param[in]    op_code   The opcode of the response.
 * @param[in]    result    The result of the operation.
 *
 * @return The length added to the buffer.
 */
static uint32_t response_prepare(uint8_t *p_buffer, uint8_t op_code, uint8_t result)
{
    ASSERT(p_buffer);
    p_buffer[0] = NRF_DFU_OP_RESPONSE;
    p_buffer[1] = op_code;
    p_buffer[2] = result;
    return RESPONSE_HEADER_LEN;
}


/**@brief Function for encoding a select object response into a buffer.
 *
 * The select object response consists of a maximum object size, a firmware offset, and a CRC value.
 *
 * @param[inout] p_buffer   The buffer to encode the response into.
 * @param[in]    max_size   The maximum object size value to encode.
 * @param[in]    fw_offset  The firmware offset value to encode.
 * @param[in]    crc        The CRC value to encode.
 *
 * @return The length added to the buffer.
 */
static uint32_t response_select_obj_add(uint8_t   *p_buffer,
                                        uint32_t   max_size,
                                        uint32_t   fw_offset,
                                        uint32_t   crc)
{
    uint16_t offset = uint32_encode(max_size,  &p_buffer[RESPONSE_HEADER_LEN]);
    offset         += uint32_encode(fw_offset, &p_buffer[RESPONSE_HEADER_LEN + offset]);
    offset         += uint32_encode(crc,       &p_buffer[RESPONSE_HEADER_LEN + offset]);
    return offset;
}


/**@brief Function for encoding a CRC response into a buffer.
 *
 * The CRC response consists of a firmware offset and a CRC value.
 *
 * @param[inout] p_buffer   The buffer to encode the response into.
 * @param[in]    fw_offset  The firmware offset value to encode.
 * @param[in]    crc        The CRC value to encode.
 *
 * @return The length added to the buffer.
 */
static uint32_t response_crc_add(uint8_t *p_buffer, uint32_t fw_offset, uint32_t crc)
{
    uint16_t offset = uint32_encode(fw_offset, &p_buffer[RESPONSE_HEADER_LEN]);
    offset         += uint32_encode(crc,       &p_buffer[RESPONSE_HEADER_LEN + offset]);
    return offset;
}


/**@brief Function for appending an extended error code to the response buffer.
 *
 * @param[inout] p_buffer    The buffer to append the extended error code to.
 * @param[in]    result      The error code to append.
 * @param[in]    buf_offset  The current length of the buffer.
 *
 * @return The length added to the buffer.
 */
static uint32_t response_ext_err_payload_add(uint8_t *p_buffer, uint8_t result, uint32_t buf_offset)
{
    p_buffer[buf_offset] = ext_error_get();
    (void) ext_error_set(NRF_DFU_EXT_ERROR_NO_ERROR);
    return 1;
}


static void ble_dfu_req_handler_callback(nrf_dfu_response_t *p_res, void *p_context)
{
    ASSERT(p_res);
    ASSERT(p_context);

    uint8_t len = 0;
    uint8_t buffer[MAX_RESPONSE_LEN] = {0};

    if (p_res->request == NRF_DFU_OP_OBJECT_WRITE) {
        --m_pkt_notif_target_cnt;
        if ((m_pkt_notif_target == 0) || (m_pkt_notif_target_cnt && m_pkt_notif_target > 0)) {
            return;
        }
        port_trace("m_pkt_notif_target_cnt:%d,m_pkt_notif_target:%d \n", m_pkt_notif_target_cnt, m_pkt_notif_target);

        /* Reply with a CRC message and reset the packet counter. */
        m_pkt_notif_target_cnt = m_pkt_notif_target;

        p_res->request = NRF_DFU_OP_CRC_GET;
    }
    //port_trace("ble_dfu_req_handler_callback \n");
    len += response_prepare(buffer, p_res->request, p_res->result);

    if (p_res->result != NRF_DFU_RES_CODE_SUCCESS) {
        port_trace("DFU request %d failed with error: 0x%x \n", p_res->request, p_res->result);

        if (p_res->result == NRF_DFU_RES_CODE_EXT_ERROR) {
            len += response_ext_err_payload_add(buffer, p_res->result, len);
        }

        (void) response_send(buffer, len);
        return;
    }

    switch (p_res->request) {
        case NRF_DFU_OP_OBJECT_CREATE:
        case NRF_DFU_OP_OBJECT_EXECUTE:
            break;

        case NRF_DFU_OP_OBJECT_SELECT: {
            len += response_select_obj_add(buffer,
                                           p_res->select.max_size,
                                           p_res->select.offset,
                                           p_res->select.crc);
        }
        break;

        case NRF_DFU_OP_OBJECT_WRITE: {
            len += response_crc_add(buffer, p_res->write.offset, p_res->write.crc);
        }
        break;

        case NRF_DFU_OP_CRC_GET: {
            len += response_crc_add(buffer, p_res->crc.offset, p_res->crc.crc);
        }
        break;

        default: {
            // No action.
        } break;
    }

    (void) response_send(buffer, len);
}


/**@brief     Function for handling a Write event on the Control Point characteristic.
 *
 * @param[in] p_dfu             DFU Service Structure.
 * @param[in] p_ble_write_evt   Pointer to the write event received from BLE stack.
 *
 * @return    NRF_SUCCESS on successful processing of control point write. Otherwise an error code.
 */
static uint32_t on_ctrl_pt_write(ble_dfu_t *p_dfu, ble_gatts_evt_write_t const *p_ble_write_evt)
{
    //lint -save -e415 -e416 : Out-of-bounds access on p_ble_write_evt->data
    nrf_dfu_request_t request = {
        .request           = (nrf_dfu_op_t)(p_ble_write_evt->data[0]),
        .p_context         = p_dfu,
        .callback.response = ble_dfu_req_handler_callback,
    };

    port_trace("[%s]request:%d \n", __FUNCTION__, request.request);
    switch (request.request) {
        case NRF_DFU_OP_OBJECT_SELECT: {
            /* Set object type to read info about */
            request.select.object_type = p_ble_write_evt->data[1];
            port_trace("object type:%d \n", request.select.object_type);
        }
        break;

        case NRF_DFU_OP_OBJECT_CREATE: {
            /* Reset the packet receipt notification on create object */
            m_pkt_notif_target_cnt = m_pkt_notif_target;

            request.create.object_type = p_ble_write_evt->data[1];
            request.create.object_size = uint32_decode(&(p_ble_write_evt->data[2]));

            if (request.create.object_type == NRF_DFU_OBJ_TYPE_COMMAND) {
                /* Activity on the current transport. Close all except the current one. */
                (void) nrf_dfu_transports_close(&ble_dfu_transport);
            }
        }
        break;

        case NRF_DFU_OP_RECEIPT_NOTIF_SET: {

            m_pkt_notif_target     = uint16_decode(&(p_ble_write_evt->data[1]));
            m_pkt_notif_target_cnt = m_pkt_notif_target;
            port_trace("Set receipt notif,%d \n", m_pkt_notif_target);
        }
        break;

        default:
            break;
    }
    //lint -restore : Out-of-bounds access

    return nrf_dfu_req_handler_on_req(&request);
}


/**@brief     Function for handling the @ref BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST event from the
 *            SoftDevice.
 *
 * @param[in] p_dfu     DFU Service Structure.
 * @param[in] p_ble_evt Pointer to the event received from BLE stack.
 */
static bool on_rw_authorize_req(ble_dfu_t *p_dfu, ble_evt_t const *p_ble_evt)
{
    uint32_t err_code;

    ble_gatts_evt_rw_authorize_request_t const *p_authorize_request;
    ble_gatts_evt_write_t                const *p_ble_write_evt;

    p_authorize_request = &(p_ble_evt->evt.gatts_evt.params.authorize_request);
    p_ble_write_evt     = &(p_ble_evt->evt.gatts_evt.params.authorize_request.request.write);

    if ((p_authorize_request->type                 != BLE_GATTS_AUTHORIZE_TYPE_WRITE)
        || (p_authorize_request->request.write.handle != p_dfu->dfu_ctrl_pt_handles.value_handle)
        || (p_authorize_request->request.write.op     != BLE_GATTS_OP_WRITE_REQ)) {
        port_trace("auth failed!type:%d,op:%d \n", p_authorize_request->type, p_authorize_request->request.write.op);
        return false;
    }

    ble_gatts_rw_authorize_reply_params_t auth_reply = {
        .type                = BLE_GATTS_AUTHORIZE_TYPE_WRITE,
        .params.write.update = 1,
        .params.write.offset = p_ble_write_evt->offset,
        .params.write.len    = p_ble_write_evt->len,
        .params.write.p_data = p_ble_write_evt->data,
    };

    if (!is_cccd_configured(p_dfu)) {
        port_trace("cccd not configured! \n");
        /* Send an error response to the peer indicating that the CCCD is improperly configured. */
        auth_reply.params.write.gatt_status = BLE_GATT_STATUS_ATTERR_CPS_CCCD_CONFIG_ERROR;

        /* Ignore response of auth reply */
        (void) sd_ble_gatts_rw_authorize_reply(m_conn_handle, &auth_reply);
        return false;
    } else {
        auth_reply.params.write.gatt_status = BLE_GATT_STATUS_SUCCESS;

        err_code = sd_ble_gatts_rw_authorize_reply(m_conn_handle, &auth_reply);
        //port_trace("on_rw_authorize_req err_code:%d \n", err_code);
        return err_code  == NRF_SUCCESS ? true : false;
    }
}


static void on_flash_write(void *p_buf)
{
    //port_trace("Freeing buffer %p \n", p_buf);
    nrf_balloc_free(&m_buffer_pool, p_buf);
}


/**@brief   Function for handling the @ref BLE_GATTS_EVT_WRITE event from the SoftDevice.
 *
 * @param[in] p_dfu     DFU Service Structure.
 * @param[in] p_ble_evt Pointer to the event received from BLE stack.
 */
static void on_write(ble_dfu_t *p_dfu, ble_evt_t const *p_ble_evt)
{
    ble_gatts_evt_write_t const *const p_write_evt = &p_ble_evt->evt.gatts_evt.params.write;

    if (p_write_evt->handle != p_dfu->dfu_pkt_handles.value_handle) {
        return;
    }

    /* Allocate a buffer to receive data. */
    uint8_t *p_balloc_buf = nrf_balloc_alloc(&m_buffer_pool);
    if (p_balloc_buf == NULL) {
        /* Operations are retried by the host; do not give up here. */
        port_trace("cannot allocate memory buffer! \n");
        return;
    }

    //port_trace("Buffer %p acquired, len %d (%d) \n", p_balloc_buf, p_write_evt->len, MAX_DFU_PKT_LEN);

    /* Copy payload into buffer. */
    memcpy(p_balloc_buf, p_write_evt->data, p_write_evt->len);

    /* Set up the request. */
    nrf_dfu_request_t request = {
        .request      = NRF_DFU_OP_OBJECT_WRITE,
        .p_context    = p_dfu,
        .callback     =
        {
            .response = ble_dfu_req_handler_callback,
            .write    = on_flash_write,
        }
    };

    /* Set up the request buffer. */
    request.write.p_data   = p_balloc_buf;
    request.write.len      = p_write_evt->len;

    /* Schedule handling of the request. */
    ret_code_t rc = nrf_dfu_req_handler_on_req(&request);
    if (rc != NRF_SUCCESS) {
        /* The error is logged in nrf_dfu_req_handler_on_req().
         * Free the buffer.
         */
        (void) nrf_balloc_free(&m_buffer_pool, p_balloc_buf);
    }
}

void dfu_ble_write_handler(uint8_t *data, uint32_t size)
{
    ble_evt_t const *p_ble_evt = (ble_evt_t const *)data;
    on_write(&m_dfu, p_ble_evt);
}

void dfu_ble_authorize_handler(uint8_t *data, uint32_t size)
{
    uint32_t err_code;
    ble_evt_t const *p_ble_evt = (ble_evt_t const *)data;

    if (p_ble_evt->evt.gatts_evt.params.authorize_request.type != BLE_GATTS_AUTHORIZE_TYPE_INVALID) {
        if (on_rw_authorize_req(&m_dfu, p_ble_evt)) {
            err_code = on_ctrl_pt_write(&m_dfu, &(p_ble_evt->evt.gatts_evt.params.authorize_request.request.write));
            if (err_code != NRF_SUCCESS) {
                port_trace("Could not handle on_ctrl_pt_write. err_code: 0x%04x \n", err_code);
            }
        }
    }
}

/**@brief       Function for adding DFU Packet characteristic to the BLE Stack.
 *
 * @param[in]   p_dfu DFU Service structure.
 *
 * @return      NRF_SUCCESS on success. Otherwise an error code.
 */
static uint32_t dfu_pkt_char_add(ble_dfu_t *const p_dfu)
{
    ble_gatts_char_md_t char_md = {
        .char_props.write_wo_resp = 1,
    };

    ble_uuid_t char_uuid = {
        .type = p_dfu->uuid_type,
        .uuid = BLE_DFU_PKT_CHAR_UUID,
    };

    ble_gatts_attr_md_t attr_md = {
        .vloc = BLE_GATTS_VLOC_STACK,
        .vlen = 1,
        .write_perm =
        {
            .sm = 1,
#if NRF_DFU_BLE_REQUIRES_BONDS
            .lv = 2,
#else
            .lv = 1,
#endif
        }
    };

    ble_gatts_attr_t attr_char_value = {
        .p_uuid    = &char_uuid,
        .p_attr_md = &attr_md,
        .max_len   = MAX_DFU_PKT_LEN,
    };

    return sd_ble_gatts_characteristic_add(p_dfu->service_handle,
                                           &char_md,
                                           &attr_char_value,
                                           &p_dfu->dfu_pkt_handles);
}


/**@brief       Function for adding DFU Control Point characteristic to the BLE Stack.
 *
 * @param[in]   p_dfu DFU Service structure.
 *
 * @return      NRF_SUCCESS on success. Otherwise an error code.
 */
static uint32_t dfu_ctrl_pt_add(ble_dfu_t *const p_dfu)
{
    ble_gatts_char_md_t char_md = {
        .char_props.write  = 1,
        .char_props.notify = 1,
    };

    ble_uuid_t char_uuid = {
        .type = p_dfu->uuid_type,
        .uuid = BLE_DFU_CTRL_PT_UUID,
    };

    ble_gatts_attr_md_t attr_md = {
        .vloc    = BLE_GATTS_VLOC_STACK,
        .wr_auth = 1,
        .vlen    = 1,
        .write_perm =
        {
            .sm = 1,
#if NRF_DFU_BLE_REQUIRES_BONDS
            .lv = 2,
#else
            .lv = 1,
#endif
        },
    };

    ble_gatts_attr_t attr_char_value = {
        .p_uuid    = &char_uuid,
        .p_attr_md = &attr_md,
        .max_len   = BLE_GATT_ATT_MTU_DEFAULT,
    };

    return sd_ble_gatts_characteristic_add(p_dfu->service_handle,
                                           &char_md,
                                           &attr_char_value,
                                           &p_dfu->dfu_ctrl_pt_handles);
}


/**@brief     Function for checking if the CCCD of DFU Control point is configured for Notification.
 *
 * @details   This function checks if the CCCD of DFU Control Point characteristic is configured
 *            for Notification by the DFU Controller.
 *
 * @param[in] p_dfu DFU Service structure.
 *
 * @return    True if the CCCD of DFU Control Point characteristic is configured for Notification.
 *            False otherwise.
 */
uint32_t ble_dfu_init(ble_dfu_t *p_dfu)
{
    ASSERT(p_dfu != NULL);

    ble_uuid_t service_uuid;
    uint32_t   err_code;

    m_conn_handle = BLE_CONN_HANDLE_INVALID;

    BLE_UUID_BLE_ASSIGN(service_uuid, BLE_DFU_SERVICE_UUID);

    err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,
                                        &service_uuid,
                                        &(p_dfu->service_handle));
    VERIFY_SUCCESS(err_code);

    ble_uuid128_t const base_uuid128 = {
        {
            0x50, 0xEA, 0xDA, 0x30, 0x88, 0x83, 0xB8, 0x9F,
            0x60, 0x4F, 0x15, 0xF3,  0x00, 0x00, 0xC9, 0x8E
        }
    };

    err_code = sd_ble_uuid_vs_add(&base_uuid128, &p_dfu->uuid_type);
    VERIFY_SUCCESS(err_code);

    err_code = dfu_pkt_char_add(p_dfu);
    VERIFY_SUCCESS(err_code);

    err_code = dfu_ctrl_pt_add(p_dfu);
    VERIFY_SUCCESS(err_code);

    return NRF_SUCCESS;
}


uint32_t ble_dfu_transport_init(nrf_dfu_observer_t observer)
{
    uint32_t err_code = NRF_SUCCESS;

    if (m_flags & DFU_BLE_FLAG_INITIALIZED) {
        return err_code;
    }

    port_trace("Initializing BLE DFU transport \n");

    err_code = nrf_balloc_init(&m_buffer_pool);
    UNUSED_RETURN_VALUE(err_code);
    /* Initialize the Device Firmware Update Service. */
    err_code = ble_dfu_init(&m_dfu);
    VERIFY_SUCCESS(err_code);

    m_flags |= DFU_BLE_FLAG_INITIALIZED;

    port_trace("BLE DFU transport initialized. \n");

    return NRF_SUCCESS;
}


uint32_t ble_dfu_transport_close(nrf_dfu_transport_t const *p_exception)
{
    uint32_t err_code = NRF_SUCCESS;

#if 0
    if ((m_flags & DFU_BLE_FLAG_INITIALIZED) && (p_exception != &ble_dfu_transport)) {
        NRF_LOG_DEBUG("Shutting down BLE transport.");

        if (m_conn_handle != BLE_CONN_HANDLE_INVALID) {
            NRF_LOG_DEBUG("Disconnecting.");

            /* Set flag to prevent advertisement from starting */
            m_flags |= DFU_BLE_RESETTING_SOON;

            /* Disconnect from the peer. */
            err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            VERIFY_SUCCESS(err_code);

            /* Wait a bit for the disconnect event to be sent on air. */
            nrf_delay_ms(200);
        } else {
            err_code = sd_ble_gap_adv_stop(m_adv_handle);
            UNUSED_RETURN_VALUE(err_code);
        }

        err_code = nrf_sdh_disable_request();
        if (err_code == NRF_SUCCESS) {
            NRF_LOG_DEBUG("BLE transport shut down.");
        }
    }
#endif
    return err_code;
}

uint32_t ble_dfu_transport_disconnect(void)
{
    uint32_t err_code = NRF_SUCCESS;

    if (m_flags & DFU_BLE_FLAG_INITIALIZED) {
        port_trace("Disconnect from BLE peer. \n");

        if (m_conn_handle != BLE_CONN_HANDLE_INVALID) {
            port_trace("Disconnecting. \n");

            /* Disconnect from the peer. */
            err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            VERIFY_SUCCESS(err_code);
        }
    }

    return err_code;
}

static void ble_dfu_evt_handler(ble_evt_t const *p_ble_evt, void *p_context)
{
    uint32_t err_code = 0;
    switch (p_ble_evt->header.evt_id) {
        case BLE_GATTS_EVT_WRITE: {
            ble_dfu_service_refresh_time();
            on_write(&m_dfu, p_ble_evt);
        }
        break;
        case BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST: {
            if (p_ble_evt->evt.gatts_evt.params.authorize_request.type
                != BLE_GATTS_AUTHORIZE_TYPE_INVALID) {
                if (on_rw_authorize_req(&m_dfu, p_ble_evt)) {
                    err_code = on_ctrl_pt_write(&m_dfu,
                                                &(p_ble_evt->evt.gatts_evt.params.authorize_request.request.write));

                    if (err_code != NRF_SUCCESS) {
                        port_trace("Could not handle on_ctrl_pt_write. err_code: 0x%04x \n", err_code);
                    }
                }
            }
        }
        break;
        default:
            // No implementation needed.
            break;
    }
}

/* Register as a BLE event observer to receive BLE events. */
NRF_SDH_BLE_OBSERVER(m_ble_evt_observer, 2, ble_dfu_evt_handler, NULL);

