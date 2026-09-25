// recorder.ino — FERRAMENTA de teste de hardware e de coleta do dataset.
//
// Este sketch NÃO é o produto final (o produto é firmware/kws_rtos, com
// FreeRTOS e 3+ tasks). Ele existe para:
//   1) testar cada componente isoladamente (LED e microfone);
//   2) gravar o dataset com o PRÓPRIO INMP441, usando exatamente a mesma
//      conversão de áudio (audio_io) que o firmware final usa.
//
// Protocolo (Serial 921600 baud, comandos terminados em '\n'):
//   PING          -> "PONG recorder"
//   LEVEL         -> imprime nível do microfone a cada 250 ms (qualquer tecla para)
//   CHAN          -> 1 s em estéreo: mostra o nível do canal esquerdo e direito
//   REC <ms>      -> grava <ms> ms (máx 2000). Responde "DATA <n>\n",
//                    depois n amostras int16 little-endian, depois "\nEND\n".
//                    O LED fica aceso durante a gravação ("fale agora").
//   LEDS          -> mostra os dois padrões do LED: acerto (1 longa) e erro (3 curtas)

#include <Arduino.h>
#include <math.h>
#include "kws_config.h"
#include "audio_io.h"

static const uint32_t kMaxRecSamples = AUDIO_SAMPLE_RATE * 2;  // 2 s
static int16_t s_rec[kMaxRecSamples];                           // 64 KB em RAM
static int16_t s_block[AUDIO_BLOCK_SAMPLES];
static bool s_audio_ok = false;

// ---------------------------------------------------------------- LED
static void led_ok() {  // acerto: 1 piscada longa
  digitalWrite(PIN_LED, HIGH); delay(LED_OK_MS);
  digitalWrite(PIN_LED, LOW);
}
static void led_fail() {  // erro: 3 piscadas curtas
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED, HIGH); delay(LED_FAIL_ON_MS);
    digitalWrite(PIN_LED, LOW);  delay(LED_FAIL_OFF_MS);
  }
}
static void led_test() {
  led_ok();
  delay(500);
  led_fail();
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
  }
  while (Serial.available()) Serial.read();
}

static void cmd_chan() {
  // Lê 1 s de quadros estéreos crus e mede a variação (desvio padrão) de cada
  // slot. O desvio padrão ignora o offset DC e mede só o "áudio" de fato.
  static int32_t raw[2 * 256];
  double sum[2] = {0, 0}, sq[2] = {0, 0};
  size_t frames = 0;
  audio_flush();
  while (frames < AUDIO_SAMPLE_RATE) {
    size_t n = audio_read_raw(raw, 256);
    if (n == 0) break;
    for (size_t i = 0; i < n; i++)
      for (int c = 0; c < 2; c++) {
        double v = (double)(raw[2 * i + c] >> 8);
        sum[c] += v;
        sq[c] += v * v;
      }
    frames += n;
  }
  for (int c = 0; c < 2; c++) {
    double mean = sum[c] / frames;
    double sd = sqrt(fmax(sq[c] / frames - mean * mean, 0.0));
    Serial.printf("CHAN slot%d: media=%.0f  variacao(sd)=%.0f (%.1f dBFS24)%s\n", c, mean, sd,
                  sd > 0 ? 20.0 * log10(sd / 8388608.0) : -120.0,
                  c == AUDIO_MIC_SLOT ? "  <- slot configurado" : "");
  }
  // Teste no caminho completo (conversão + DC blocker + ganho), o que o firmware final usa:
  double sumsq = 0; int peak = 0; size_t total = 0;
  audio_flush();
  while (total < AUDIO_SAMPLE_RATE / 2) {
    size_t n = audio_read_block(s_block, AUDIO_BLOCK_SAMPLES);
    block_stats(s_block, n, sumsq, peak);
    total += n;
  }
  Serial.printf("CHAN modo configurado: rms %.1f dBFS peak %.1f dBFS (%s)\n",
                to_dbfs(sqrt(sumsq / total)), to_dbfs(peak),
                peak > 0 ? "OK, ha sinal" : "SEM SINAL - troque AUDIO_MIC_SLOT em kws_config.h");
}

static void cmd_rec(uint32_t ms) {
  if (ms == 0 || ms > 2000) ms = REC_CLIP_MS;
  uint32_t n = (uint32_t)((uint64_t)ms * AUDIO_SAMPLE_RATE / 1000);
  audio_flush();  // joga fora áudio antigo acumulado no DMA
  // descarta ~32 ms para o filtro DC estabilizar após a pausa
  for (int i = 0; i < 2; i++) audio_read_block(s_block, AUDIO_BLOCK_SAMPLES);
  digitalWrite(PIN_LED, HIGH);  // "fale agora"
  uint32_t got = 0;
  while (got < n) {
    size_t want = min((uint32_t)AUDIO_BLOCK_SAMPLES, n - got);
    size_t r = audio_read_block(s_rec + got, want);
    if (r == 0) break;
    got += r;
  }
  digitalWrite(PIN_LED, LOW);
  Serial.printf("DATA %lu\n", (unsigned long)got);
  Serial.write((const uint8_t *)s_rec, got * sizeof(int16_t));
  Serial.print("\nEND\n");
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  Serial.setTxBufferSize(4096);
  Serial.begin(921600);
  pinMode(PIN_LED, OUTPUT);
  delay(300);
  led_test();
  s_audio_ok = audio_begin();
  Serial.printf("\nrecorder pronto. audio=%s  sr=%d  shift=%d\n",
                s_audio_ok ? "OK" : "FALHOU", AUDIO_SAMPLE_RATE, AUDIO_SHIFT_24_TO_16);
  if (!s_audio_ok) digitalWrite(PIN_LED, HIGH);  // aceso fixo = erro no microfone
}

void loop() {
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
