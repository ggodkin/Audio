#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"  // Modern I2S API
#include "esp_bt.h"          // BT Core
#include "esp_bt_main.h"     // Bluedroid
#include "esp_a2dp_api.h"    // A2DP
#include "esp_bt_device.h"   // Device Control

// I2S Configuration
#define I2S_PORT        I2S_NUM_0
#define I2S_BCK_IO      14
#define I2S_WS_IO       15
#define I2S_DO_IO       22

// Global I2S Channel Handle
i2s_chan_handle_t tx_chan;

void a2dp_data_handler(const uint8_t *data, uint32_t len) {
    size_t bytes_written;
    i2s_channel_write(tx_chan, data, len, &bytes_written, portMAX_DELAY);
}

void app_main() {
    // 1. Initialize I2S with modern API
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = -1,
            .mclk = -1
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    // 2. Initialize Bluetooth
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // 3. Configure A2DP
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(a2dp_data_handler));
    ESP_ERROR_CHECK(esp_a2d_sink_init());

    // 4. Set device name
    ESP_ERROR_CHECK(esp_bt_dev_set_device_name("ESP32-A2DP"));

    // 5. Enable discovery
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    while(1) vTaskDelay(pdMS_TO_TICKS(1000));
}