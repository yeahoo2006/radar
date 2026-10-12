#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ble_link.h"

#define RADAR_UART UART_NUM_2
#define RADAR_RX GPIO_NUM_16
#define RADAR_TX GPIO_NUM_17
#define RADAR_OT1 GPIO_NUM_14

static const char *TAG = "LD2451";
static const uint8_t header[] = {0xf4, 0xf3, 0xf2, 0xf1};
static const uint8_t footer[] = {0xf8, 0xf7, 0xf6, 0xf5};
static uint8_t payload[127];
static unsigned state, matched, length, index_in_frame;
static unsigned frame_count, byte_count;
static uint8_t radar_config[8];
static uint8_t config_valid_flags;

static void read_radar_configuration(void);
static void apply_radar_configuration(const uint8_t desired[8], uint8_t transaction_id);

static void reset_frame(void) {
    state = matched = length = index_in_frame = 0;
}

static void print_frame(void) {
    frame_count++;
    if (length == 0) {
        ble_link_publish(0, 0, gpio_get_level(RADAR_OT1), NULL);
        ESP_LOGI(TAG, "frame=%u empty/no target; OT1=%d", frame_count, gpio_get_level(RADAR_OT1));
        return;
    }
    if (length < 2 || length != 2u + 5u * payload[0]) {
        ESP_LOGW(TAG, "frame=%u unexpected length=%u", frame_count, length);
        return;
    }
    ble_link_publish(payload[0], payload[1], gpio_get_level(RADAR_OT1), payload + 2);
    ESP_LOGI(TAG, "frame=%u targets=%u approaching_alarm=%u OT1=%d",
             frame_count, payload[0], payload[1], gpio_get_level(RADAR_OT1));
    for (unsigned i = 0; i < payload[0]; ++i) {
        unsigned at = 2u + 5u * i;
        ESP_LOGI(TAG, "target=%u angle=%d deg distance=%u m direction=%s speed=%u km/h SNR=%u",
                 i + 1, (int)payload[at] - 128, payload[at + 1],
                 payload[at + 2] == 1 ? "approaching" : "receding",
                 payload[at + 3], payload[at + 4]);
    }
}

static void consume(uint8_t value) {
    switch (state) {
    case 0: // Header
        matched = value == header[matched] ? matched + 1 : (value == header[0] ? 1 : 0);
        if (matched == 4) { state = 1; matched = 0; }
        break;
    case 1: // Payload length, little endian
        length = value;
        state = 2;
        break;
    case 2:
        length |= (unsigned)value << 8;
        if (length > sizeof(payload)) reset_frame();
        else { index_in_frame = matched = 0; state = length ? 3 : 4; }
        break;
    case 3: // Payload
        payload[index_in_frame++] = value;
        if (index_in_frame == length) state = 4;
        break;
    case 4: // Footer
        if (value != footer[matched]) {
            reset_frame();
            if (value == header[0]) matched = 1;
        } else if (++matched == 4) {
            print_frame();
            reset_frame();
        }
        break;
    }
}

void app_main(void) {
    const uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(RADAR_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(RADAR_UART, RADAR_TX, RADAR_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(RADAR_UART, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(gpio_set_direction(RADAR_OT1, GPIO_MODE_INPUT));
    ESP_LOGI(TAG, "test started: RX2=GPIO16 TX2=GPIO17 OT1=GPIO14 baud=115200");
    read_radar_configuration();
    ble_link_start(radar_config, config_valid_flags);

    int last_ot1 = -1;
    TickType_t last_status = xTaskGetTickCount();
    uint8_t data[128];
    while (1) {
        int n = uart_read_bytes(RADAR_UART, data, sizeof(data), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; ++i) consume(data[i]);
        byte_count += n;
        int ot1 = gpio_get_level(RADAR_OT1);
        if (ot1 != last_ot1) {
            ESP_LOGI(TAG, "OT1/GPIO14=%d", ot1);
            last_ot1 = ot1;
        }
        if (xTaskGetTickCount() - last_status >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "status: uart_bytes=%u valid_frames=%u OT1=%d",
                     byte_count, frame_count, ot1);
            last_status = xTaskGetTickCount();
        }
        uint8_t desired[8], transaction_id;
        if (ble_link_take_config_request(desired, &transaction_id))
            apply_radar_configuration(desired, transaction_id);
    }
}

// Only the two queries (0x0012, 0x0013) read stored parameters.
// Enable/end commands temporarily enter and exit the module's command mode.
static bool command(uint16_t code, const uint8_t *value, unsigned value_len,
                    uint8_t *result, unsigned *result_len) {
    uint8_t tx[16] = {0xfd, 0xfc, 0xfb, 0xfa, 0, 0,
                      (uint8_t)code, (uint8_t)(code >> 8)};
    tx[4] = 2 + value_len;
    for (unsigned i = 0; i < value_len; ++i) tx[8 + i] = value[i];
    unsigned end = 8 + value_len;
    tx[end] = 0x04; tx[end + 1] = 0x03;
    tx[end + 2] = 0x02; tx[end + 3] = 0x01;
    uart_write_bytes(RADAR_UART, tx, end + 4);
    uart_wait_tx_done(RADAR_UART, pdMS_TO_TICKS(100));

    const uint8_t hdr[] = {0xfd, 0xfc, 0xfb, 0xfa};
    const uint8_t tail[] = {0x04, 0x03, 0x02, 0x01};
    uint8_t rx[32];
    unsigned phase = 0, match = 0, len = 0, pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    while (xTaskGetTickCount() < deadline) {
        uint8_t b;
        if (uart_read_bytes(RADAR_UART, &b, 1, pdMS_TO_TICKS(50)) != 1) continue;
        switch (phase) {
        case 0:
            match = b == hdr[match] ? match + 1 : (b == hdr[0] ? 1 : 0);
            if (match == 4) { phase = 1; match = 0; }
            break;
        case 1: len = b; phase = 2; break;
        case 2:
            len |= (unsigned)b << 8;
            if (len < 4 || len > sizeof(rx)) { phase = match = 0; }
            else { phase = 3; pos = 0; }
            break;
        case 3:
            rx[pos++] = b;
            if (pos == len) { phase = 4; match = 0; }
            break;
        case 4:
            if (b != tail[match]) { phase = match = 0; break; }
            if (++match == 4) {
                uint16_t response_code = rx[0] | ((uint16_t)rx[1] << 8);
                uint16_t status = rx[2] | ((uint16_t)rx[3] << 8);
                if (response_code == (code | 0x0100) && status == 0) {
                    *result_len = len - 4;
                    for (unsigned i = 0; i < *result_len; ++i) result[i] = rx[4 + i];
                    return true;
                }
                phase = match = 0;
            }
            break;
        }
    }
    ESP_LOGE(TAG, "command 0x%04x: no successful ACK", code);
    return false;
}

static void read_radar_configuration(void) {
    uint8_t data[28];
    unsigned len = 0;
    const uint8_t enable[] = {0x01, 0x00};
    uart_flush_input(RADAR_UART);
    if (!command(0x00ff, enable, 2, data, &len)) return;
    ESP_LOGI(TAG, "configuration mode enabled");

    len = 0;
    if (command(0x0012, NULL, 0, data, &len) && len == 4) {
        memcpy(radar_config, data, 4);
        config_valid_flags |= 0x01;
        const char *direction = data[1] == 0 ? "receding only" :
                                data[1] == 1 ? "approaching only" :
                                data[1] == 2 ? "both" : "unknown";
        ESP_LOGI(TAG, "CONFIG detection: max_distance=%u m direction=%u (%s) min_speed=%u km/h no_target_delay=%u s",
                 data[0], data[1], direction, data[2], data[3]);
        ESP_LOGI(TAG, "CONFIG detection raw: %02X %02X %02X %02X",
                 data[0], data[1], data[2], data[3]);
    } else ESP_LOGE(TAG, "detection parameter response invalid, length=%u", len);

    len = 0;
    if (command(0x0013, NULL, 0, data, &len) && len == 4) {
        memcpy(radar_config + 4, data, 4);
        config_valid_flags |= 0x02;
        ESP_LOGI(TAG, "CONFIG sensitivity: trigger_count=%u SNR_setting=%u (0=default 4) extra=%u,%u",
                 data[0], data[1], data[2], data[3]);
        ESP_LOGI(TAG, "CONFIG sensitivity raw: %02X %02X %02X %02X",
                 data[0], data[1], data[2], data[3]);
    } else ESP_LOGE(TAG, "sensitivity response invalid, length=%u", len);

    len = 0;
    if (command(0x00fe, NULL, 0, data, &len))
        ESP_LOGI(TAG, "configuration mode ended; normal reports resumed");
}

static void apply_radar_configuration(const uint8_t desired[8], uint8_t transaction_id) {
    uint8_t data[28];
    unsigned len = 0;
    uint8_t actual[8];
    memcpy(actual, radar_config, sizeof(actual));
    uint8_t valid = 0;
    uint8_t failed_stage = 0;
    const uint8_t enable[] = {0x01, 0x00};
    ESP_LOGI(TAG, "applying BLE configuration transaction %u", transaction_id);
    uart_flush_input(RADAR_UART);

    if (!command(0x00ff, enable, 2, data, &len)) {
        failed_stage = 0xff;
        goto done;
    }
    len = 0;
    if (!command(0x0002, desired, 4, data, &len)) {
        failed_stage = 0x02;
    } else {
        len = 0;
        if (!command(0x0003, desired + 4, 4, data, &len))
            failed_stage = 0x03;
    }

    // Query both groups even after a failed write: the two radar write
    // commands are separate and may have left a partial change.
    len = 0;
    if (command(0x0012, NULL, 0, data, &len) && len == 4) {
        memcpy(actual, data, 4);
        valid |= 0x01;
    } else if (failed_stage == 0) failed_stage = 0x12;

    len = 0;
    if (command(0x0013, NULL, 0, data, &len) && len == 4) {
        memcpy(actual + 4, data, 4);
        valid |= 0x02;
    } else if (failed_stage == 0) failed_stage = 0x13;

    len = 0;
    if (!command(0x00fe, NULL, 0, data, &len) && failed_stage == 0)
        failed_stage = 0xfe;

done:
    memcpy(radar_config, actual, sizeof(actual));
    config_valid_flags = valid;
    uint8_t result_code = failed_stage ? 4 : 2;
    if (result_code == 2 && memcmp(actual, desired, sizeof(actual)) != 0) {
        result_code = 5;
        failed_stage = memcmp(actual, desired, 4) != 0 ? 0x12 : 0x13;
    }
    if (result_code == 2)
        ESP_LOGI(TAG, "BLE configuration applied and verified: %02X %02X %02X %02X %02X %02X %02X %02X",
                 actual[0], actual[1], actual[2], actual[3],
                 actual[4], actual[5], actual[6], actual[7]);
    else
        ESP_LOGE(TAG, "BLE configuration failed: code=%u stage=0x%02X valid=0x%02X",
                 result_code, failed_stage, valid);
    ble_link_finish_config(transaction_id, result_code, failed_stage,
                           actual, valid);
}
