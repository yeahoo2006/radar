#include "ble_link.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);

static const char *TAG = "LD2451_BLE";
static const char *DEVICE_NAME = "LD2451-ESP32";
static uint8_t addr_type;
static uint16_t live_handle;
static uint16_t result_handle;
static _Atomic uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static _Atomic bool subscribed = false;
static _Atomic bool result_subscribed = false;
static uint8_t config_value[10];
static uint8_t result_value[13] = {1};
static uint8_t pending_config[8];
static uint8_t pending_id;
static bool config_pending;
static bool config_busy;
static uint8_t latest_summary[12] = {1, 0x10};
static uint16_t sequence;
static uint32_t last_notify_ms;
static portMUX_TYPE summary_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE config_lock = portMUX_INITIALIZER_UNLOCKED;

static bool valid_config(const uint8_t *data) {
    return data[0] >= 10 && data[0] <= 100 && data[1] <= 2 &&
           data[2] <= 120 && data[4] >= 1 && data[4] <= 10 &&
           (data[5] == 0 || (data[5] >= 3 && data[5] <= 8)) &&
           data[6] == 0 && data[7] == 0;
}

static int access_cb(uint16_t conn, uint16_t attr,
                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (arg == (void *)3 && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) != 10) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t request[10];
        if (os_mbuf_copydata(ctxt->om, 0, 10, request) != 0)
            return BLE_ATT_ERR_UNLIKELY;
        if (request[0] != 1 || !valid_config(request + 2))
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        portENTER_CRITICAL(&config_lock);
        if (config_busy) {
            portEXIT_CRITICAL(&config_lock);
            return BLE_ATT_ERR_UNLIKELY;
        }
        memcpy(pending_config, request + 2, 8);
        pending_id = request[1];
        config_pending = config_busy = true;
        result_value[0] = 1;
        result_value[1] = pending_id;
        result_value[2] = 1; // Applying.
        result_value[3] = 0;
        result_value[4] = config_value[1];
        memcpy(result_value + 5, config_value + 2, 8);
        portEXIT_CRITICAL(&config_lock);
        ESP_LOGI(TAG, "config transaction %u queued", request[1]);
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    if (arg == (void *)1) {
        uint8_t copy[sizeof(latest_summary)];
        portENTER_CRITICAL(&summary_lock);
        memcpy(copy, latest_summary, sizeof(copy));
        portEXIT_CRITICAL(&summary_lock);
        return os_mbuf_append(ctxt->om, copy, sizeof(copy)) == 0 ?
               0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (arg == (void *)2) {
        uint8_t copy[sizeof(config_value)];
        portENTER_CRITICAL(&config_lock);
        memcpy(copy, config_value, sizeof(copy));
        portEXIT_CRITICAL(&config_lock);
        return os_mbuf_append(ctxt->om, copy, sizeof(copy)) == 0 ?
               0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (arg == (void *)4) {
        uint8_t copy[sizeof(result_value)];
        portENTER_CRITICAL(&config_lock);
        memcpy(copy, result_value, sizeof(copy));
        portEXIT_CRITICAL(&config_lock);
        return os_mbuf_append(ctxt->om, copy, sizeof(copy)) == 0 ?
               0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// UUIDs are written low byte first in the NimBLE initializer.
// Human-readable base: 3a56c3b0-0c7a-4fd9-8e9e-0d2451000000.
static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(0x00, 0x00, 0x00, 0x51, 0x24, 0x0d,
                                    0x9e, 0x8e, 0xd9, 0x4f, 0x7a, 0x0c,
                                    0xb0, 0xc3, 0x56, 0x3a),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID128_DECLARE(0x01, 0x00, 0x00, 0x51, 0x24, 0x0d,
                                            0x9e, 0x8e, 0xd9, 0x4f, 0x7a, 0x0c,
                                            0xb0, 0xc3, 0x56, 0x3a),
                .access_cb = access_cb,
                .arg = (void *)1,
                .val_handle = &live_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID128_DECLARE(0x02, 0x00, 0x00, 0x51, 0x24, 0x0d,
                                            0x9e, 0x8e, 0xd9, 0x4f, 0x7a, 0x0c,
                                            0xb0, 0xc3, 0x56, 0x3a),
                .access_cb = access_cb,
                .arg = (void *)2,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID128_DECLARE(0x03, 0x00, 0x00, 0x51, 0x24, 0x0d,
                                            0x9e, 0x8e, 0xd9, 0x4f, 0x7a, 0x0c,
                                            0xb0, 0xc3, 0x56, 0x3a),
                .access_cb = access_cb,
                .arg = (void *)3,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = BLE_UUID128_DECLARE(0x04, 0x00, 0x00, 0x51, 0x24, 0x0d,
                                            0x9e, 0x8e, 0xd9, 0x4f, 0x7a, 0x0c,
                                            0xb0, 0xc3, 0x56, 0x3a),
                .access_cb = access_cb,
                .arg = (void *)4,
                .val_handle = &result_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            atomic_store(&conn_handle, event->connect.conn_handle);
            atomic_store(&subscribed, false);
            atomic_store(&result_subscribed, false);
            ESP_LOGI(TAG, "phone connected, conn=%u", event->connect.conn_handle);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        atomic_store(&subscribed, false);
        atomic_store(&result_subscribed, false);
        atomic_store(&conn_handle, BLE_HS_CONN_HANDLE_NONE);
        ESP_LOGI(TAG, "phone disconnected");
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == live_handle) {
            atomic_store(&subscribed, event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "live notifications %s",
                     event->subscribe.cur_notify ? "enabled" : "disabled");
        } else if (event->subscribe.attr_handle == result_handle) {
            atomic_store(&result_subscribed, event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "config result notifications %s",
                     event->subscribe.cur_notify ? "enabled" : "disabled");
        }
        break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "advertisement data failed: %d", rc); return; }
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "advertising failed: %d", rc);
    else ESP_LOGI(TAG, "advertising as %s", DEVICE_NAME);
}

static void on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "address setup failed: %d", rc); return; }
    advertise();
}

static void host_task(void *arg) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_link_start(const uint8_t config[8], uint8_t valid_flags) {
    config_value[0] = 1;
    config_value[1] = valid_flags;
    memcpy(config_value + 2, config, 8);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(DEVICE_NAME));
    nimble_port_freertos_init(host_task);
}

static bool notify_packet(uint16_t conn, uint16_t handle,
                          const uint8_t *data, size_t len) {
    struct os_mbuf *packet = ble_hs_mbuf_from_flat(data, len);
    if (!packet) return false;
    int rc = ble_gatts_notify_custom(conn, handle, packet);
    if (rc != 0) {
        ESP_LOGW(TAG, "notification failed: %d", rc);
        return false;
    }
    return true;
}

void ble_link_publish(uint8_t count, uint8_t alarm, uint8_t ot1,
                      const uint8_t *targets) {
    if (count > 5) count = 5;
    uint32_t now = esp_log_timestamp();
    uint16_t seq = ++sequence;
    uint8_t summary[12] = {
        1, 0x10, (uint8_t)seq, (uint8_t)(seq >> 8),
        count, alarm, ot1, 0,
        (uint8_t)now, (uint8_t)(now >> 8),
        (uint8_t)(now >> 16), (uint8_t)(now >> 24),
    };
    portENTER_CRITICAL(&summary_lock);
    memcpy(latest_summary, summary, sizeof(summary));
    portEXIT_CRITICAL(&summary_lock);

    if (!atomic_load(&subscribed) || now - last_notify_ms < 200) return;
    uint16_t conn = atomic_load(&conn_handle);
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    last_notify_ms = now;
    if (!notify_packet(conn, live_handle, summary, sizeof(summary))) return;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t target[10] = {
            1, 0x11, (uint8_t)seq, (uint8_t)(seq >> 8), i,
            targets[5 * i], targets[5 * i + 1], targets[5 * i + 2],
            targets[5 * i + 3], targets[5 * i + 4],
        };
        if (!notify_packet(conn, live_handle, target, sizeof(target))) break;
    }
}

bool ble_link_take_config_request(uint8_t desired[8], uint8_t *transaction_id) {
    portENTER_CRITICAL(&config_lock);
    bool ready = config_pending;
    if (ready) {
        memcpy(desired, pending_config, 8);
        *transaction_id = pending_id;
        config_pending = false;
    }
    portEXIT_CRITICAL(&config_lock);
    return ready;
}

void ble_link_finish_config(uint8_t transaction_id, uint8_t result_code,
                            uint8_t failed_stage, const uint8_t actual[8],
                            uint8_t valid_flags) {
    uint8_t result[13] = {1, transaction_id, result_code, failed_stage, valid_flags};
    memcpy(result + 5, actual, 8);
    portENTER_CRITICAL(&config_lock);
    config_value[1] = valid_flags;
    memcpy(config_value + 2, actual, 8);
    memcpy(result_value, result, sizeof(result));
    config_busy = false;
    portEXIT_CRITICAL(&config_lock);
    ESP_LOGI(TAG, "config transaction %u result=%u stage=0x%02x valid=0x%02x",
             transaction_id, result_code, failed_stage, valid_flags);
    uint16_t conn = atomic_load(&conn_handle);
    if (atomic_load(&result_subscribed) && conn != BLE_HS_CONN_HANDLE_NONE)
        notify_packet(conn, result_handle, result, sizeof(result));
}
