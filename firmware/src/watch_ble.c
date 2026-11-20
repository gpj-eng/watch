#include "watch_board.h"
#include <rtthread.h>
#include <string.h>
#include "bf0_ble_gap.h"
#include "bf0_sibles.h"
#include "bf0_sibles_internal.h"
#include "bf0_sibles_advertising.h"
#include "ble_connection_manager.h"

#define SERIAL_UUID_16(value) {(uint8_t)((value) & 0xff), (uint8_t)((value) >> 8)}
#define NUS_UUID(id) {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, id, 0x00, 0x40, 0x6e}
enum {SVC, RX_CHAR, RX_VALUE, TX_CHAR, TX_VALUE, TX_CCCD, ATT_COUNT};
static uint8_t service_uuid[16] = NUS_UUID(1);
static sibles_hdl svc_handle;
static volatile uint8_t conn = 0xff;
static volatile uint16_t cccd;
static volatile bool powered, authenticated, pair_open;
static volatile uint32_t pair_started;
static volatile unsigned pair_pin = 1000000;
static rt_mq_t acknowledgements;
SIBLES_ADVERTISING_CONTEXT_DECLAR(advertisement);

BLE_GATT_SERVICE_DEFINE_128(watch_att_db)
{
    BLE_GATT_SERVICE_DECLARE(SVC, SERIAL_UUID_16_PRI_SERVICE, BLE_GATT_PERM_READ_ENABLE),
    BLE_GATT_CHAR_DECLARE(RX_CHAR, SERIAL_UUID_16_CHARACTERISTIC, BLE_GATT_PERM_READ_ENABLE),
    BLE_GATT_CHAR_VALUE_DECLARE(RX_VALUE, NUS_UUID(2),
        BLE_GATT_PERM_WRITE_REQ_ENABLE | BLE_GATT_PERM_WRITE_PERMISSION_AUTH,
        BLE_GATT_VALUE_PERM_UUID_128 | BLE_GATT_VALUE_PERM_RI_ENABLE, 20),
    BLE_GATT_CHAR_DECLARE(TX_CHAR, SERIAL_UUID_16_CHARACTERISTIC, BLE_GATT_PERM_READ_ENABLE),
    BLE_GATT_CHAR_VALUE_DECLARE(TX_VALUE, NUS_UUID(3),
        BLE_GATT_PERM_NOTIFY_ENABLE | BLE_GATT_PERM_NOTIFY_PERMISSION_AUTH,
        BLE_GATT_VALUE_PERM_UUID_128 | BLE_GATT_VALUE_PERM_RI_ENABLE, 20),
    BLE_GATT_DESCRIPTOR_DECLARE(TX_CCCD, SERIAL_UUID_16_CLIENT_CHAR_CFG,
        BLE_GATT_PERM_READ_ENABLE | BLE_GATT_PERM_WRITE_REQ_ENABLE | BLE_GATT_PERM_WRITE_PERMISSION_AUTH,
        BLE_GATT_VALUE_PERM_RI_ENABLE, 2),
};

static bool pairing_allowed(void)
{
    return pair_open && watch_now_ms() - pair_started < 60000u;
}

static uint8_t *get_value(uint8_t connection, uint8_t index, uint16_t *len)
{
    static uint8_t subscription[2];
    *len = 0;
    if (connection != conn || index != TX_CCCD)
        return NULL;
    subscription[0] = (uint8_t)cccd;
    subscription[1] = 0;
    *len = sizeof(subscription);
    return subscription;
}

static uint8_t set_value(uint8_t connection, sibles_set_cbk_t *p)
{
    if (connection != conn)
        return SIBLES_ATT_ERR_WRITE_NOT_PERMITTED;
    if (p->idx == TX_CCCD && p->len == 2) {
        cccd = p->value[0] & 1;
        return 0;
    }
    if (p->idx == RX_VALUE && p->len == 20 && authenticated)
        return rt_mq_send(acknowledgements, p->value, 20) == RT_EOK ? 0 : 0x80;
    return SIBLES_ATT_ERR_WRITE_NOT_PERMITTED;
}

static uint8_t adv_event(uint8_t event, void *context, void *data)
{
    (void)event;
    (void)context;
    (void)data;
    return 0;
}

static void start_advertisement(void)
{
    static const char name[] = "HSPI-SOS";
    BLE_GATT_SERVICE_INIT_128(svc, watch_att_db, ATT_COUNT,
        BLE_GATT_SERVICE_PERM_NOAUTH | BLE_GATT_SERVICE_PERM_UUID_128, service_uuid);
    svc_handle = sibles_register_svc_128(&svc);
    if (!svc_handle)
        return;
    sibles_register_cbk(svc_handle, get_value, set_value);
    connection_manager_set_bond_ack(BOND_PENDING);
    connection_manager_set_bond_cnf_iocap(GAP_IO_CAP_DISPLAY_ONLY);
    connection_manager_set_bond_cnf_auth(GAP_AUTH_MITM | GAP_AUTH_BOND | GAP_AUTH_SEC_CON);
    connection_manager_set_bond_cnf_sec(GAP_SEC1_AUTH_PAIR_ENC);

    sibles_advertising_para_t adv = {0};
    adv.own_addr_type = GAPM_STATIC_ADDR;
    adv.config.adv_mode = SIBLES_ADV_CONNECT_MODE;
    adv.config.mode_config.conn_config.interval = 0x40;
    adv.config.is_auto_restart = 1;
    adv.config.max_tx_pwr = 0x7f;
    adv.adv_data.completed_name = rt_malloc(sizeof(sibles_adv_type_name_t) + sizeof(name) - 1);
    if (!adv.adv_data.completed_name)
        return;
    adv.adv_data.completed_name->name_len = sizeof(name) - 1;
    memcpy(adv.adv_data.completed_name->name, name, sizeof(name) - 1);
    adv.evt_handler = adv_event;
    if (sibles_advertising_init(advertisement, &adv) == SIBLES_ADV_NO_ERR)
        sibles_advertising_start(advertisement);
}

static int ble_event(uint16_t event, uint8_t *data, uint16_t len, uint32_t context)
{
    (void)len;
    (void)context;
    switch (event) {
    case BLE_POWER_ON_IND:
        powered = true;
        break;
    case BLE_GAP_CONNECTED_IND:
        if (conn == 0xff) {
            authenticated = false;
            cccd = 0;
            conn = ((ble_gap_connect_ind_t *)data)->conn_idx;
        }
        break;
    case BLE_GAP_DISCONNECTED_IND:
        if (((ble_gap_disconnected_ind_t *)data)->conn_idx == conn) {
            authenticated = false;
            conn = 0xff;
            cccd = 0;
            pair_pin = 1000000;
        }
        break;
    case BLE_GAP_ENCRYPT_IND: {
        ble_gap_encrypt_ind_t *ind = (void *)data;
        if (ind->conn_idx == conn) {
            authenticated = (ind->auth & (GAP_AUTH_MITM | GAP_AUTH_BOND)) == (GAP_AUTH_MITM | GAP_AUTH_BOND);
            if (authenticated) {
                pair_open = false;
                pair_pin = 1000000;
            }
        }
        break;
    }
    case CONNECTION_MANAGER_BOND_AUTH_INFOR: {
        connection_manager_bond_ack_infor_t *info = (void *)data;
        bool accept = info->conn_idx == conn && pairing_allowed();
        if (info->request == GAPC_TK_EXCH) {
            accept = accept && info->type == GAP_TK_DISPLAY;
            if (accept)
                pair_pin = info->confirm_data;
        }
        if (info->request == GAPC_NC_EXCH)
            accept = false;
        connection_manager_bond_ack_reply(info->conn_idx, info->request, accept);
        break;
    }
    default:
        break;
    }
    return 0;
}
BLE_EVENT_REGISTER(ble_event, NULL);

void watch_ble_init(void)
{
    acknowledgements = rt_mq_create("sosack", 20, 8, RT_IPC_FLAG_FIFO);
    if (acknowledgements)
        sifli_ble_enable();
}

void watch_ble_poll(void)
{
    static bool started;
    if (powered && !started) {
        started = true;
        start_advertisement();
    }
    if (pair_open && !pairing_allowed()) {
        pair_open = false;
        pair_pin = 1000000;
    }
}

bool watch_ble_ready(void)
{
    return svc_handle && conn != 0xff && authenticated && cccd;
}

bool watch_ble_send(uint8_t frame[20])
{
    if (!watch_ble_ready())
        return false;
    sibles_value_t data = {.hdl = svc_handle, .idx = TX_VALUE, .len = 20, .value = frame};
    return sibles_write_value(conn, &data) == 0;
}

bool watch_ble_ack(uint64_t *id, uint8_t *status)
{
    uint8_t frame[20];
    return acknowledgements && rt_mq_recv(acknowledgements, frame, sizeof(frame), 0) == RT_EOK &&
           watch_frame_ack(frame, sizeof(frame), id, status);
}

void watch_ble_pair(void)
{
    pair_started = watch_now_ms();
    pair_open = true;
}

unsigned watch_ble_pin(void)
{
    return pair_pin;
}
