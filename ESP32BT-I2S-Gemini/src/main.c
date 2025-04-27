#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "driver/i2s.h"

static const char* TAG = "A2DP_SINK";

// I2S Configuration
#define I2S_PORT I2S_NUM_0
#define PIN_BCLK 14
#define PIN_WS 15
#define PIN_DOUT 22

// Bluetooth Device Name
static const char* DEVICE_NAME = "ESP32BT-I2S";

static void i2s_init() {
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN,
        .sample_rate = 44100, // Will be updated by A2DP
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .tx_desc_auto_clear = true // Auto clear tx descriptor if under run
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = PIN_BCLK,
        .ws_io_num = PIN_WS,
        .data_out_num = PIN_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_config));
    ESP_ERROR_CHECK(i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN));
}

// A2DP Callback Functions
static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            esp_a2d_cb_param_conn_stat_t *conn_stat = (esp_a2d_cb_param_conn_stat_t *)param;
            ESP_LOGI(TAG, "A2DP connection state: %s",
                     conn_stat->state == ESP_A2D_CONNECTION_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED");
            if (conn_stat->state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "A2DP connected to: %s", esp_bd_addr_to_str(conn_stat->remote_bda));
            } else if (conn_stat->state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGI(TAG, "A2DP disconnected from: %s", esp_bd_addr_to_str(conn_stat->remote_bda));
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            esp_a2d_cb_param_audio_stat_t *audio_stat = (esp_a2d_cb_param_audio_stat_t *)param;
            ESP_LOGI(TAG, "A2DP audio state: %s",
                     audio_stat->state == ESP_A2D_AUDIO_STATE_STARTED ? "STARTED" : "STOPPED");
            if (audio_stat->state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "Audio stream started");
            } else if (audio_stat->state == ESP_A2D_AUDIO_STATE_STOPPED) {
                ESP_LOGI(TAG, "Audio stream stopped");
            }
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT: {
            esp_a2d_cb_param_audio_cfg_t *audio_cfg = (esp_a2d_cb_param_audio_cfg_t *)param;
            ESP_LOGI(TAG, "A2DP audio stream configuration, sample rate=%d bits=%d channels=%d",
                     audio_cfg->sample_rate, audio_cfg->bits_per_sample, audio_cfg->channel_count);
            i2s_set_sample_rates(I2S_PORT, audio_cfg->sample_rate);
            break;
        }
        default:
            ESP_LOGI(TAG, "A2DP callback event: %d", event);
            break;
    }
}

// A2DP Data Callback
static void audio_data_cb(const uint8_t* data, uint32_t len) {
    size_t bytes_written = 0;
    i2s_write(I2S_PORT, data, len, &bytes_written, portMAX_DELAY);
    if (bytes_written != len) {
        ESP_LOGW(TAG, "I2S write incomplete: %d written vs %d expected", bytes_written, len);
    }
}

void app_main(void) {
    esp_err_t ret;

    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "Initializing Bluetooth...");

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller memory release failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller initialization failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller enablement failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid initialization failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid enablement failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_ERROR_CHECK(esp_bt_dev_set_device_name(DEVICE_NAME));

    // Initialize A2DP sink
    esp_a2d_register_callback(a2dp_cb);
    esp_a2d_sink_register_data_callback(audio_data_cb);
    esp_a2d_sink_init();

    // Initialize I2S
    i2s_init();

    ESP_LOGI(TAG, "A2DP Sink initialized. Device name: %s", DEVICE_NAME);
}