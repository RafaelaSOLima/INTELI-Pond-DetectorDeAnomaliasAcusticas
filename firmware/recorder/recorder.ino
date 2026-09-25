// recorder.ino — FERRAMENTA de teste de hardware e de coleta do dataset.
//
// Este sketch NÃO é o produto final (o produto é firmware/kws_rtos, com
// FreeRTOS e 3+ tasks). Ele existe para:
//   1) testar cada componente isoladamente (LEDs, botão, microfone);
//   2) gravar o dataset com o PRÓPRIO INMP441, usando exatamente a mesma
//      conversão de áudio (audio_io) que o firmware final usa.
//
// Protocolo (Serial 921600 baud, comandos terminados em '\n'):
//   PING          -> "PONG recorder"
//   LEVEL         -> imprime nível do microfone a cada 250 ms (qualquer tecla para)
//   CHAN          -> 1 s em estéreo: mostra o nível do canal esquerdo e direito
//   REC <ms>      -> grava <ms> ms (máx 2000). Responde "DATA <n>\n",
//                    depois n amostras int16 little-endian, depois "\nEND\n".
//                    O LED verde fica aceso durante a gravação ("fale agora").
//   LEDS          -> pisca verde/vermelho (teste dos LEDs)
// Botão: cada pressionamento imprime "BTN" (teste do botão + debounce).

#include <Arduino.h>
#include <math.h>
#include "kws_config.h"
#include "audio_io.h"

static const uint32_t kMaxRecSamples = AUDIO_SAMPLE_RATE * 2;  // 2 s
static int16_t s_rec[kMaxRecSamples];                           // 64 KB em RAM
static int16_t s_block[AUDIO_BLOCK_SAMPLES];
static bool s_audio_ok = false;

// ---------------------------------------------------------------- LEDs
static void led_test() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(PIN_LED_GREEN, HIGH); delay(250);
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, HIGH);   delay(250);
    digitalWrite(PIN_LED_RED, LOW);
  }
}

// ---------------------------------------------------------------- botão
// Debounce por tempo: a leitura só é aceita se ficar estável por 30 ms.
// (Contatos mecânicos "quicam" várias vezes em poucos ms ao serem pressionados.)
static void poll_button() {
  static int stable = HIGH, last_raw = HIGH;
  static uint32_t t_change = 0;
  int raw = digitalRead(PIN_BUTTON);  // pull-up: solto = HIGH, pressionado = LOW
  if (raw != last_raw) { last_raw = raw; t_change = millis(); }
  if (millis() - t_change > 30 && raw != stable) {
    stable = raw;
    if (stable == LOW) Serial.println("BTN");
  }
}

// ---------------------------------------------------------------- microfone
static void block_stats(const int16_t *x, size_t n, double &sumsq, int &peak) {
  for (size_t i = 0; i < n; i++) {
    sumsq += (double)x[i] * x[i];
    int a = abs((int)x[i]);
    if (a > peak) peak = a;
  }
}

static float to_dbfs(double v) { return v <= 0 ? -120.0f : 20.0f * log10f((float)(v / 32768.0)); }

static void cmd_level() {
  Serial.println("LEVEL: fale perto do microfone. Envie qualquer tecla para parar.");
  audio_flush();
  while (!Serial.available()) {
    double sumsq = 0; int peak = 0; size_t total = 0;
    while (total < AUDIO_SAMPLE_RATE / 4) {  // 250 ms
      size_t n = audio_read_block(s_block, AUDIO_BLOCK_SAMPLES);
      block_stats(s_block, n, sumsq, peak);
      total += n;
    }
    float rms_db = to_dbfs(sqrt(sumsq / total));
    int bars = (int)((rms_db + 70.0f) / 2.0f);
    if (bars < 0) bars = 0;
    if (bars > 35) bars = 35;
    Serial.printf("rms %6.1f dBFS  peak %6.1f dBFS  |", rms_db, to_dbfs(peak));
    for (int i = 0; i < bars; i++) Serial.print('#');
    Serial.println();
    poll_button();
  }
  while (Serial.available()) Serial.read();
}

static void cmd_chan() {
  // Reinstala o driver em estéreo por 1 s para ver em qual canal o mic responde.
  audio_end();
  audio_begin(I2S_CHANNEL_FMT_RIGHT_LEFT);
  static int32_t raw[2 * 256];
  double s[2] = {0, 0}; long nz[2] = {0, 0}; size_t frames = 0;
  while (frames < AUDIO_SAMPLE_RATE) {
    size_t n = audio_read_raw(raw, 2 * 256);
    for (size_t i = 0; i + 1 < n; i += 2) {
      for (int c = 0; c < 2; c++) {
        double v = (double)(raw[i + c] >> 8);
        s[c] += v * v;
        if (raw[i + c] != 0) nz[c]++;
      }
      frames++;
    }
  }
  // Na ordem do driver ESP32, o índice 0 é o slot "right" e o 1 o slot "left"
  // em alguns casos — por isso mostramos os dois e decidimos pelo resultado.
  Serial.printf("CHAN slot0: rms24=%.0f nonzero=%ld | slot1: rms24=%.0f nonzero=%ld\n",
                sqrt(s[0] / frames), nz[0], sqrt(s[1] / frames), nz[1]);
  audio_end();
  s_audio_ok = audio_begin(AUDIO_CHANNEL_FMT);
  // Teste adicional no modo configurado (o que o firmware final usa):
  double sumsq = 0; int peak = 0; size_t total = 0;
  audio_flush();
  while (total < AUDIO_SAMPLE_RATE / 2) {
    size_t n = audio_read_block(s_block, AUDIO_BLOCK_SAMPLES);
    block_stats(s_block, n, sumsq, peak);
    total += n;
  }
  Serial.printf("CHAN modo configurado: rms %.1f dBFS peak %.1f dBFS (%s)\n",
                to_dbfs(sqrt(sumsq / total)), to_dbfs(peak),
                peak > 0 ? "OK, ha sinal" : "SEM SINAL - troque o canal em kws_config.h");
}

static void cmd_rec(uint32_t ms) {
  if (ms == 0 || ms > 2000) ms = REC_CLIP_MS;
  uint32_t n = (uint32_t)((uint64_t)ms * AUDIO_SAMPLE_RATE / 1000);
  audio_flush();  // joga fora áudio antigo acumulado no DMA
  // descarta ~32 ms para o filtro DC estabilizar após a pausa
  for (int i = 0; i < 2; i++) audio_read_block(s_block, AUDIO_BLOCK_SAMPLES);
  digitalWrite(PIN_LED_GREEN, HIGH);  // "fale agora"
  uint32_t got = 0;
  while (got < n) {
    size_t want = min((uint32_t)AUDIO_BLOCK_SAMPLES, n - got);
    size_t r = audio_read_block(s_rec + got, want);
    if (r == 0) break;
    got += r;
  }
  digitalWrite(PIN_LED_GREEN, LOW);
  Serial.printf("DATA %lu\n", (unsigned long)got);
  Serial.write((const uint8_t *)s_rec, got * sizeof(int16_t));
  Serial.print("\nEND\n");
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  Serial.setTxBufferSize(4096);
  Serial.begin(921600);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  delay(300);
  led_test();
  s_audio_ok = audio_begin();
  Serial.printf("\nrecorder pronto. audio=%s  sr=%d  shift=%d\n",
                s_audio_ok ? "OK" : "FALHOU", AUDIO_SAMPLE_RATE, AUDIO_SHIFT_24_TO_16);
  if (!s_audio_ok) digitalWrite(PIN_LED_RED, HIGH);
}

void loop() {
  poll_button();
  if (!Serial.available()) { delay(2); return; }
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line == "PING") Serial.println("PONG recorder");
  else if (line == "LEVEL") cmd_level();
  else if (line == "CHAN") cmd_chan();
  else if (line == "LEDS") { led_test(); Serial.println("LEDS ok"); }
  else if (line.startsWith("REC")) cmd_rec((uint32_t)line.substring(3).toInt());
  else if (line.length()) Serial.printf("ERR comando desconhecido: %s\n", line.c_str());
}
