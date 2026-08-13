// ESP-IDF (v5.x) voice recorder / playback toy.
//
// Hardware:
//   INMP441 mic : WS=GPIO20  SD=GPIO18  SCK=GPIO19
//   Passive buzzer / small speaker : GPIO1 (LEDC PWM)
//   Heartbeat LED (just for fun)   : GPIO0, toggles every 300 ms
//   Button : GPIO9 (Xiao ESP32-C6 onboard BOOT button, active-low, no
//            external wiring needed). Change BUTTON_GPIO if you'd rather
//            wire a dedicated button elsewhere.
//
// Behaviour: press the button to record 5s of audio; press it again to
// play that recording back through the buzzer; press again to record over
// it, and so on (alternates record/play on each press).
//
// CMakeLists.txt (main component):
//   idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
//       REQUIRES esp_driver_i2s esp_driver_ledc esp_driver_gpio esp_timer)

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/sdm.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

// ---------- Pin map ----------
#define I2S_WS_PIN   20
#define I2S_SD_PIN   18
#define I2S_SCK_PIN  19
#define BUZZER_GPIO  GPIO_NUM_1
#define LED_GPIO     GPIO_NUM_0
#define BUTTON_GPIO  GPIO_NUM_21

// 8 kHz keeps the math clean (125 us/sample, exact for esp_timer) and is
// plenty for intelligible voice over a PWM "DAC".
#define SAMPLE_RATE     8000
#define RECORD_SECONDS  5
#define RECORD_SAMPLES  (SAMPLE_RATE * RECORD_SECONDS)   // 40000 samples, 78 KB

// Saturating casts: keep gained-up values from wrapping around instead of
// clipping cleanly, which sounds like a pop instead of a harsh screech.
static inline int16_t clamp16(int32_t v) {
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

static inline int8_t clamp8(int32_t v) {
    if (v > 127) return 127;
    if (v < -128) return -128;
    return (int8_t)v;
}

static const char *TAG = "voice_toy";

// Static buffer: 40000 * 2 bytes stays comfortably inside the C6's 512 KB SRAM.
static int16_t s_recording_buf[RECORD_SAMPLES];

typedef enum { APP_IDLE, APP_RECORDING, APP_PLAYING } app_state_t;
static volatile app_state_t s_state = APP_IDLE;
static bool s_has_recording = false;

static i2s_chan_handle_t s_rx_chan;
static sdm_channel_handle_t s_sdm_chan;
static esp_timer_handle_t s_playback_timer;
static volatile size_t s_playback_index;
static QueueHandle_t s_button_evt_queue;

// ============================================================
// I2S microphone (new "std mode" driver)
// Reference: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html
// ============================================================
static void i2s_mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; // INMP441 L/R tied to GND

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
}

// Sigma-Delta Modulation "DAC" driving the passive buzzer
// Reference: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/peripherals/sdm.html
static void buzzer_init(void)
{
    sdm_config_t sdm_cfg = {
        .clk_src        = SDM_CLK_SRC_DEFAULT,
        .gpio_num       = BUZZER_GPIO,
        .sample_rate_hz = 1000000, // SDM's own oversample rate, unrelated to our 8 kHz audio rate
    };
    ESP_ERROR_CHECK(sdm_new_channel(&sdm_cfg, &s_sdm_chan));
    ESP_ERROR_CHECK(sdm_channel_enable(s_sdm_chan));
    ESP_ERROR_CHECK(sdm_channel_set_pulse_density(s_sdm_chan, 0)); // start silent
}

static void IRAM_ATTR playback_tick_cb(void *arg)
{
    if (s_playback_index >= RECORD_SAMPLES) {
        sdm_channel_set_pulse_density(s_sdm_chan, 0); // back to silence
        esp_timer_stop(s_playback_timer);
        s_has_recording = false; // force next idle press to record, not replay
        s_state = APP_IDLE;
        return;
    }

    int16_t sample = s_recording_buf[s_playback_index++];
    // >>6 instead of >>8 is a 4x playback boost on top of the record-side
    // gain above; clamp8 catches anything that overshoots ±127.
    int8_t density = clamp8(sample >> 6);
    sdm_channel_set_pulse_density(s_sdm_chan, density);
}

static void playback_timer_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = playback_tick_cb,
        .dispatch_method = ESP_TIMER_ISR,   
        .name = "playback_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_playback_timer));
}

static void start_playback(void)
{
    s_playback_index = 0;
    s_state = APP_PLAYING;
    ESP_LOGI(TAG, "Playing back %d s of audio", RECORD_SECONDS);
    // 1,000,000 us / 8000 Hz = 125 us exactly -> no rounding/drift error.
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_playback_timer, 1000000ULL / SAMPLE_RATE));
}

// ============================================================
// Recording: plain blocking task, same read pattern as the level-meter.
// ============================================================
static void record_task(void *arg)
{
    ESP_LOGI(TAG, "Recording %d s...", RECORD_SECONDS);
    for (int i = 0; i < RECORD_SAMPLES; i++) {
        int32_t raw = 0;
        size_t bytes_read = 0;
        i2s_channel_read(s_rx_chan, &raw, sizeof(raw), &bytes_read, portMAX_DELAY);
        // INMP441 gives a 24-bit sample left-justified in the 32-bit word;
        // >>16 keeps only the most-significant 16 bits, which safely fits
        // int16_t range with no clipping. Shift less (e.g. >>13) for a
        // louder but clip-prone signal.
        // >>11 instead of >>16 is ~32x louder capture. Push toward >>9/>>10
        // for more gain, back toward >>14 if you hear clipping/crackle.
        s_recording_buf[i] = clamp16(raw >> 11);
    }
    s_has_recording = true;
    s_state = APP_IDLE;
    ESP_LOGI(TAG, "Recording done, press the button to play it back");
    vTaskDelete(NULL);
}
// ============================================================
// Send the recording to the host as base64 over the same serial
// link used for logging, wrapped in plain-text markers so a host
// script can find it even inside idf.py monitor's log noise.
// Reference: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mbedtls.html
// ============================================================
#define B64_CHUNK_RAW_BYTES 240 // multiple of 3 -> only the final chunk needs '=' padding

static void dump_recording_b64(void)
{
    printf("---AUDIO-START---\n");

    const uint8_t *raw = (const uint8_t *)s_recording_buf;
    size_t total = RECORD_SAMPLES * sizeof(int16_t);
    size_t offset = 0;
    unsigned char out[B64_CHUNK_RAW_BYTES * 4 / 3 + 16];

    while (offset < total) {
        size_t chunk = (total - offset < B64_CHUNK_RAW_BYTES) ? (total - offset) : B64_CHUNK_RAW_BYTES;
        size_t out_len = 0;
        int ret = mbedtls_base64_encode(out, sizeof(out), &out_len, raw + offset, chunk);
        if (ret != 0) {
            ESP_LOGE(TAG, "base64 encode failed: %d", ret);
            return;
        }
        out[out_len] = '\0';
        printf("%s\n", out);
        offset += chunk;
    }

    printf("---AUDIO-END---\n");
}
// Switches stdin from the default non-blocking, poll-only console reads to
// the UART driver's interrupt-driven, blocking reads, so a single incoming
// byte can't be missed between polls.
// Reference: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/stdio.html
static void console_stdin_init(void)
{
    if (uart_is_driver_installed(UART_NUM_0)) {
        return;
    }
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(UART_NUM_0);
}
// Polls stdin (non-blocking by default on the console VFS) for a 'd'
// keypress/byte sent by the host script, and streams the recording
// out when it arrives.
static void serial_dump_task(void *arg)
{
    for (;;) {
        int c = fgetc(stdin); // now blocks until a byte actually arrives
        if (c == 'd' || c == 'D') {
            if (s_state != APP_IDLE) {
                ESP_LOGW(TAG, "Busy, try again once record/playback finishes");
            } else if (!s_has_recording) {
                ESP_LOGW(TAG, "No recording yet, press the button first");
            } else {
                dump_recording_b64();
            }
        }
    }
}
// ============================================================
// Heartbeat LED, purely decorative, runs independently of app state.
// ============================================================
static void led_task(void *arg)
{
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    bool on = false;
    for (;;) {
        on = !on;
        gpio_set_level(LED_GPIO, on);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

// ============================================================
// Button: ISR just pushes an event, all logic happens in app_main's loop.
// Reference: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/gpio.html
// ============================================================
static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t dummy = 0;
    BaseType_t higher_prio_woken = pdFALSE;
    xQueueSendFromISR(s_button_evt_queue, &dummy, &higher_prio_woken);
    if (higher_prio_woken) {
        portYIELD_FROM_ISR();
    }
}

static void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE, // active-low button to GND
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL));
}

void app_main(void)
{
    s_button_evt_queue = xQueueCreate(4, sizeof(uint32_t));

    button_init();
    i2s_mic_init();
    buzzer_init();
    playback_timer_init();
    xTaskCreate(led_task, "led_task", 2048, NULL, 3, NULL);
    console_stdin_init();
    xTaskCreate(serial_dump_task, "serial_dump_task", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Ready. Press the button to record %d s.", RECORD_SECONDS);

    uint32_t evt;
    for (;;) {
        if (xQueueReceive(s_button_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_state != APP_IDLE) {
            continue; // busy recording/playing, ignore the press
        }

        vTaskDelay(pdMS_TO_TICKS(30)); // let contact bounce settle
        xQueueReset(s_button_evt_queue);

        if (!s_has_recording) {
            s_state = APP_RECORDING;
            xTaskCreate(record_task, "record_task", 4096, NULL, 5, NULL);
        } else {
            start_playback();
        }
    }
}