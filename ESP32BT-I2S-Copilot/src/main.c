#include "driver/i2s.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_a2dp_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "APP"

// I2S Configuration
void setup_i2s() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = 14,       // BCLK pin
        .ws_io_num = 15,        // LRC pin
        .data_out_num = 22,     // DIN pin
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));
}

// Bluetooth A2DP Event Callback
void bt_a2dp_event_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "Connection state: %d", param->conn_stat.state);
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            ESP_LOGI(TAG, "Audio state: %d", param->audio_stat.state);
            break;
        default:
            ESP_LOGI(TAG, "Unhandled event: %d", event);
            break;
    }
}

// Bluetooth Data Callback
void bt_a2dp_data_cb(const uint8_t *data, uint32_t len) {
    size_t written;
    i2s_write(I2S_NUM_0, data, len, &written, portMAX_DELAY);
}

// Bluetooth Setup
void setup_bluetooth() {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_a2d_register_callback(bt_a2dp_event_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(bt_a2dp_data_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_init());
}

void app_main() {
    ESP_LOGI(TAG, "Starting application...");
    setup_i2s();
    setup_bluetooth();

    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}
