#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdlib.h>
#include "driver/i2s_std.h"
#include "esp_log.h"

#define LED_GPIO        GPIO_NUM_0
#define LED_PERIOD_MS   200
#define I2S_WS   20
#define I2S_SD   18
#define I2S_SCK  19
 
static const char *TAG = "inmp441";
static i2s_chan_handle_t rx_chan;

static void i2s_mic_init(void)
{
    // I2S_CHANNEL_DEFAULT_CONFIG: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html#i2s-channel-configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));
 
    i2s_std_config_t std_cfg = {
        // I2S_STD_CLK_DEFAULT_CONFIG: sets mclk_multiple/sample rate source
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        // Mono, 32-bit slot width (INMP441 outputs 24-bit data left-justified
        // in a 32-bit frame, same as the legacy I2S_BITS_PER_SAMPLE_32BIT).
        // I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG:
        // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html#id8
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // not used by the INMP441
            .bclk = I2S_SCK,
            .ws   = I2S_WS,
            .dout = I2S_GPIO_UNUSED,   // RX only, no data-out pin
            .din  = I2S_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    // INMP441 with L/R tied to GND transmits on the left slot, matching the
    // old I2S_CHANNEL_FMT_ONLY_LEFT setting.
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
 
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
}
static void level_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();
 
    for (;;) {
        long soma = 0;
 
        for (int i = 0; i < 100; i++) {
            int32_t sample = 0;
            size_t bytes_read = 0;
 
            // i2s_channel_read (replaces legacy i2s_read):
            // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html#read-data-from-rx-channel
            i2s_channel_read(rx_chan, &sample, sizeof(sample), &bytes_read, portMAX_DELAY);
            soma += labs(sample);
        }
 
        long media = soma / 100;
        int nivel = (int)((media * 10) / 10000000); // same 0..10000000 -> 0..10 scale as before
        if (nivel < 0)  nivel = 0;
        if (nivel > 10) nivel = 10;
        
        ESP_LOGI(TAG, "Nível %d", nivel);
        //printf("%d\n", nivel); // Serial Plotter-style output over the console UART
 
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(20));
    }
}
// Toggles the LED every LED_PERIOD_MS. Runs as its own FreeRTOS task so it
// is never blocked by the DS18B20 conversion delay in the other task.
// vTaskDelayUntil keeps a stable period regardless of loop execution time:
// https://www.freertos.org/vtaskdelayuntil.html
static void led_blink_task(void *arg)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    bool level = false;
    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        level = !level;
        gpio_set_level(LED_GPIO, level);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LED_PERIOD_MS));
    }
}


void app_main(void)
{
    i2s_mic_init();
    // unblocking FreeRTOS tasks
    xTaskCreate(level_task, "level_task", 4096, NULL, 5, NULL);
    xTaskCreate(led_blink_task, "led_blink_task", 2048, NULL, 5, NULL);
    
}