#include "BluetoothA2DPSink.h"
#include "driver/rmt.h"
#include <Arduino.h>

// Define PDM output pins
#define PDM_LEFT_PIN GPIO_NUM_25  // GPIO pin for left channel PDM
#define PDM_RIGHT_PIN GPIO_NUM_26 // GPIO pin for right channel PDM

// Create an instance of the A2DP sink
BluetoothA2DPSink a2dp_sink;

// RMT channels for PDM output
rmt_channel_t rmt_channel_left = RMT_CHANNEL_0;
rmt_channel_t rmt_channel_right = RMT_CHANNEL_1;

// Audio data callback function
void audio_data_callback(const uint8_t *data, uint32_t len) {
    static uint32_t callback_count = 0;
    callback_count++;
    Serial.printf("Audio callback called: %u, len: %u\n", callback_count, len);

    // The audio data is in 16-bit stereo format (2 channels, 16 bits per sample)
    int16_t *samples = (int16_t *)data;
    uint32_t sample_count = len / 4; // Each sample is 4 bytes (2 channels * 16 bits)

    // Allocate buffers for RMT items on the heap
    rmt_item32_t *rmt_items_left = (rmt_item32_t *)malloc(sample_count * sizeof(rmt_item32_t));
    rmt_item32_t *rmt_items_right = (rmt_item32_t *)malloc(sample_count * sizeof(rmt_item32_t));

    if (!rmt_items_left || !rmt_items_right) {
        Serial.println("Failed to allocate memory for RMT items!");
        return;
    }

    for (uint32_t i = 0; i < sample_count; i++) {
        // Extract left and right channel samples
        int16_t left_sample = samples[2 * i];
        int16_t right_sample = samples[2 * i + 1];

        // Generate PDM pulses for left channel
        rmt_items_left[i].duration0 = (left_sample > 0) ? 1 : 0;
        rmt_items_left[i].level0 = 1;
        rmt_items_left[i].duration1 = 1;
        rmt_items_left[i].level1 = 0;

        // Generate PDM pulses for right channel
        rmt_items_right[i].duration0 = (right_sample > 0) ? 1 : 0;
        rmt_items_right[i].level0 = 1;
        rmt_items_right[i].duration1 = 1;
        rmt_items_right[i].level1 = 0;
    }

    // Transmit PDM pulses using RMT
    rmt_write_items(rmt_channel_left, rmt_items_left, sample_count, false);
    rmt_write_items(rmt_channel_right, rmt_items_right, sample_count, false);

    // Free the allocated memory
    free(rmt_items_left);
    free(rmt_items_right);
}

void setup() {
    // Initialize serial for debugging
    Serial.begin(115200);
    Serial.println("Starting setup...");

    // Configure RMT for PDM output
    rmt_config_t rmt_config_left = RMT_DEFAULT_CONFIG_TX(PDM_LEFT_PIN, rmt_channel_left);
    rmt_config_left.clk_div = 1; // Set clock divider for 2.8 MHz PDM clock
    rmt_config(&rmt_config_left);
    rmt_driver_install(rmt_channel_left, 0, 0);

    rmt_config_t rmt_config_right = RMT_DEFAULT_CONFIG_TX(PDM_RIGHT_PIN, rmt_channel_right);
    rmt_config_right.clk_div = 1; // Set clock divider for 2.8 MHz PDM clock
    rmt_config(&rmt_config_right);
    rmt_driver_install(rmt_channel_right, 0, 0);

    Serial.println("RMT configured.");

    // Start Bluetooth A2DP sink
    a2dp_sink.set_stream_reader(audio_data_callback);
    a2dp_sink.start("ESP32_PDM_Audio");

    Serial.println("Bluetooth A2DP sink started.");
}

void loop() {
    // Main loop does nothing, as audio is handled in the callback
    delay(1000);
}