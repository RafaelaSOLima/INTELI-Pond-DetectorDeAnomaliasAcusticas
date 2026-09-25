#include "audio_io.h"

static const i2s_port_t kPort = I2S_NUM_0;

// Estado do filtro DC blocker (persistente entre blocos).
static float s_dc_x1 = 0.0f;
static float s_dc_y1 = 0.0f;

bool audio_begin() { return audio_begin_pins(PIN_I2S_SCK, PIN_I2S_WS, PIN_I2S_SD); }

bool audio_begin_pins(int sck, int ws, int sd) {

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);  // ESP32 gera os clocks e recebe
  cfg.sample_rate = AUDIO_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;         // INMP441: 24 bits em slots de 32
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;       // estéreo: lemos os 2 slots
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;    // padrão Philips I2S
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = I2S_DMA_BUF_COUNT;
  cfg.dma_buf_len = I2S_DMA_BUF_LEN;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;

  if (i2s_driver_install(kPort, &cfg, 0, NULL) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = sck;
  pins.ws_io_num = ws;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = sd;
  if (i2s_set_pin(kPort, &pins) != ESP_OK) return false;

  i2s_zero_dma_buffer(kPort);
  s_dc_x1 = s_dc_y1 = 0.0f;
  return true;
}

void audio_end() { i2s_driver_uninstall(kPort); }

size_t audio_read_raw(int32_t *raw, size_t n, TickType_t timeout) {
  size_t got_bytes = 0, want = n * 2 * sizeof(int32_t);  // 2 palavras por quadro
  while (got_bytes < want) {
    size_t br = 0;
    // i2s_read bloqueia até o DMA ter dados (a interrupção do I2S libera a task).
    i2s_read(kPort, (uint8_t *)raw + got_bytes, want - got_bytes, &br, timeout);
    if (br == 0) break;  // timeout
    got_bytes += br;
  }
  return got_bytes / (2 * sizeof(int32_t));
}

void audio_convert(const int32_t *raw, int16_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    // Os 24 bits úteis estão alinhados à esquerda: >> 8 recupera o valor com sinal.
    float x = (float)(raw[2 * i + AUDIO_MIC_SLOT] >> 8);
    // DC blocker (passa-altas de 1ª ordem, corte ~13 Hz a 16 kHz).
    float y = x - s_dc_x1 + AUDIO_DC_BLOCK_R * s_dc_y1;
    s_dc_x1 = x;
    s_dc_y1 = y;
    // 24 -> 16 bits com ganho fixo e saturação (evita "dar a volta" no int16).
    // Ex.: SHIFT=5 -> divide por 32 (em vez de 256), ou seja, ganho de 8x.
    float v = y / (float)(1 << AUDIO_SHIFT_24_TO_16);
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    out[i] = (int16_t)lrintf(v);
  }
}

size_t audio_read_block(int16_t *out, size_t n, TickType_t timeout) {
  static int32_t raw[2 * AUDIO_BLOCK_SAMPLES];
  if (n > AUDIO_BLOCK_SAMPLES) n = AUDIO_BLOCK_SAMPLES;
  size_t got = audio_read_raw(raw, n, timeout);
  audio_convert(raw, out, got);
  return got;
}

void audio_flush() {
  static int32_t trash[2 * I2S_DMA_BUF_LEN];
  size_t br = 0;
  do {
    br = 0;
    i2s_read(kPort, trash, sizeof(trash), &br, 0);
  } while (br > 0);
}
