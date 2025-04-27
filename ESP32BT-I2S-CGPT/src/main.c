#include <stdio.h>
#include "driver/i2s.h"

#define I2S_NUM         I2S_NUM_0
#define SAMPLE_RATE     16000
#define I2S_BCK_IO      26
#define I2S_WS_IO       25
#define I2S_DO_IO       22
#define I2S_DI_IO       -1   // Not used

void init_i2s(void)
{
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = I2S_WS_IO,
        .data_out_num = I2S_DO_IO,
        .data_in_num = I2S_DI_IO
    };

    // Install and start I2S driver
    i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM, &pin_config);
    i2s_zero_dma_buffer(I2S_NUM);
}

void app_main(void)
{
    init_i2s();

    uint16_t sample_data[16];
    for (int i = 0; i < 16; i++) {
        sample_data[i] = i * 200;
    }

    size_t bytes_written;
    while (1) {
        i2s_write(I2S_NUM, sample_data, sizeof(sample_data), &bytes_written, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
