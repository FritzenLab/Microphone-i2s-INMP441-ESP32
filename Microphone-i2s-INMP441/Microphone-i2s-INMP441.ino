// Reference: https://www.makerhero.com/blog/microfone-omnidirecional-inmp441-com-esp32/
#include "driver/i2s.h"

// =========================
// INMP441
// =========================
#define I2S_WS   20
#define I2S_SD   18
#define I2S_SCK  19
// pins above were arbitrarily selected on my Xiao ESP32-C6 for i2s,
// proving that the GPIO Matrix really works. That was mentioned
// here https://documentation.espressif.com/esp32-c6_technical_reference_manual_en.pdf#i2s

long soma = 0;
unsigned long sampleTime = 0;

void setup() {

 Serial.begin(115200);

 // =========================
 // Configuração I2S
 // =========================
 i2s_config_t i2s_config = {
   .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
   .sample_rate = 16000,
   .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
   .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
   .communication_format = I2S_COMM_FORMAT_I2S,
   .intr_alloc_flags = 0,
   .dma_buf_count = 4,
   .dma_buf_len = 64,
   .use_apll = false,
   .tx_desc_auto_clear = false,
   .fixed_mclk = 0
 };

 i2s_pin_config_t pin_config = {
   .bck_io_num = I2S_SCK,
   .ws_io_num = I2S_WS,
   .data_out_num = I2S_PIN_NO_CHANGE,
   .data_in_num = I2S_SD
 };

 i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
 i2s_set_pin(I2S_NUM_0, &pin_config);
 i2s_zero_dma_buffer(I2S_NUM_0);
 
}

void loop() {

  if(millis() - sampleTime >= 20){
    sampleTime += 20;

    soma = 0;
    // Faz média de várias amostras
    for (int i = 0; i < 100; i++) {

      int32_t sample = 0;
      size_t bytes_read;

      i2s_read(
        I2S_NUM_0,
        &sample,
        sizeof(sample),
        &bytes_read,
        portMAX_DELAY
      );

      sample = abs(sample);

      soma += sample;
    }

    long media = soma / 100;
    // Ajusta escala
    int nivel = map(media, 0, 10000000, 0, 10);
    nivel = constrain(nivel, 0, 10); 
    //Serial.println(media);
    Serial.println(nivel);
  }
}