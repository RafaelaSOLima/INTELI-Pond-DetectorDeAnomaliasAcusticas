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
//   WIRE          -> teste elétrico: procura curtos entre SCK/WS/SD, GND e 3V3
//   SCAN          -> testa as 6 combinações de SCK/WS/SD e mostra qual dá áudio
//   VOLT          -> mede tensão (ADC) em GPIO34 e GPIO35 (ligue ali VDD e GND do mic)

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

// ---------------------------------------------------------------- teste de fiação
// Desliga o I2S e usa os 3 pinos do microfone como GPIO comuns:
//  - cada pino, sozinho com pull-up interno, deve ler 1 (se ler 0: preso no GND);
//  - com pull-down interno, deve ler 0 (se ler 1: preso no 3V3);
//  - um pino é forçado a 0/1 e os outros não podem "seguir" (se seguirem: curto).
// Com o I2S parado o INMP441 fica em repouso e não interfere (SD em alta impedância).
static void cmd_wire() {
  const int pins[3] = {PIN_I2S_SCK, PIN_I2S_WS, PIN_I2S_SD};
  const char *names[3] = {"SCK(26)", "WS(25)", "SD(33)"};
  int problems = 0;
  audio_end();
  for (int i = 0; i < 3; i++) {
    pinMode(pins[i], INPUT_PULLUP);   delay(5);
    int up = digitalRead(pins[i]);
    pinMode(pins[i], INPUT_PULLDOWN); delay(5);
    int down = digitalRead(pins[i]);
    if (up == LOW)  { Serial.printf("WIRE %s preso no GND (curto com GND?)\n", names[i]); problems++; }
    if (down == HIGH) { Serial.printf("WIRE %s preso no 3V3 (curto com 3V3?)\n", names[i]); problems++; }
  }
  for (int d = 0; d < 3; d++) {
    for (int lvl = 0; lvl < 2; lvl++) {
      pinMode(pins[d], OUTPUT);
      digitalWrite(pins[d], lvl);
      for (int o = 0; o < 3; o++) {
        if (o == d) continue;
        pinMode(pins[o], lvl ? INPUT_PULLDOWN : INPUT_PULLUP);  // puxa para o nível oposto
        delay(5);
        if (digitalRead(pins[o]) == lvl && o > d) {
          Serial.printf("WIRE curto entre %s e %s\n", names[d], names[o]);
          problems++;
        }
      }
    }
    pinMode(pins[d], INPUT);
  }
  Serial.printf("WIRE fim: %s\n", problems ? "PROBLEMA(S) ACIMA" : "nenhum curto detectado nos pinos do microfone");
  s_audio_ok = audio_begin();
}

// ---------------------------------------------------------------- voltímetro
// Usa o ADC do ESP32 como voltímetro simples. Ligue um jumper da fileira do
// VDD do INMP441 até o GPIO34 e outro da fileira do GND do INMP441 até o GPIO35.
// Esperado: GPIO34 ~3000–3300 mV (o ADC satura perto de 3,1 V) e GPIO35 ~0 mV.
static void cmd_volt() {
  const int pins[2] = {34, 35};
  const char *what[2] = {"GPIO34 (VDD do mic)", "GPIO35 (GND do mic)"};
  for (int i = 0; i < 2; i++) {
    uint32_t acc = 0;
    for (int k = 0; k < 32; k++) acc += analogReadMilliVolts(pins[i]);
    Serial.printf("VOLT %s: %lu mV\n", what[i], (unsigned long)(acc / 32));
  }
}

// ---------------------------------------------------------------- varredura de pinos
// Testa as 6 formas de atribuir SCK/WS/SD aos GPIOs 26/25/33 e mede cada slot.
// Áudio plausível do INMP441: variação entre ~-90 e ~-20 dBFS24 (nem zero,
// nem saturado). Para limitar a corrente caso uma saída do ESP32 "brigue" com a
// saída SD do microfone, a força dos pinos é reduzida ao mínimo durante o teste.
static float slot_sd_db(const int32_t *raw, size_t frames, int slot) {
  double sum = 0, sq = 0;
  for (size_t i = 0; i < frames; i++) {
    double v = (double)(raw[2 * i + slot] >> 8);
    sum += v; sq += v * v;
  }
  double mean = sum / frames, sd = sqrt(fmax(sq / frames - mean * mean, 0.0));
  return sd > 0 ? 20.0f * log10f((float)(sd / 8388608.0)) : -120.0f;
}

static void cmd_scan() {
  const int g[3] = {PIN_I2S_SCK, PIN_I2S_WS, PIN_I2S_SD};
  const int perm[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
  static int32_t raw[2 * 256];
  Serial.println("SCAN: fale continuamente perto do microfone durante o teste (~5 s)");
  audio_end();
  for (int p = 0; p < 6; p++) {
    int sck = g[perm[p][0]], ws = g[perm[p][1]], sd = g[perm[p][2]];
    if (!audio_begin_pins(sck, ws, sd)) { Serial.println("SCAN falha ao iniciar I2S"); continue; }
    gpio_set_drive_capability((gpio_num_t)sck, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)ws, GPIO_DRIVE_CAP_0);
    delay(100);
    audio_flush();
    float best[2] = {-120, -120}, worst[2] = {0, 0};
    for (int k = 0; k < 24; k++) {  // ~0,4 s
      size_t n = audio_read_raw(raw, 256, pdMS_TO_TICKS(200));
      if (n == 0) break;
      for (int c = 0; c < 2; c++) {
        float db = slot_sd_db(raw, n, c);
        if (db > best[c]) best[c] = db;
        if (db < worst[c]) worst[c] = db;
      }
    }
    audio_end();
    for (int c = 0; c < 2; c++) {
      const char *verdict = best[c] <= -119 ? "zero"
                          : worst[c] > -15 ? "SATURADO/lixo"
                          : best[c] < -95 ? "quase zero"
                          : "PLAUSIVEL";
      Serial.printf("SCAN SCK=%d WS=%d SD=%d slot%d: %6.1f..%6.1f dBFS24  %s%s\n", sck, ws, sd, c,
                    worst[c], best[c], verdict,
                    (sck == PIN_I2S_SCK && ws == PIN_I2S_WS && sd == PIN_I2S_SD) ? "  (ligacao esperada)" : "");
    }
  }
  for (int i = 0; i < 3; i++) gpio_set_drive_capability((gpio_num_t)g[i], GPIO_DRIVE_CAP_2);
  s_audio_ok = audio_begin();
  Serial.println("SCAN fim");
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
  else if (line == "WIRE") cmd_wire();
  else if (line == "VOLT") cmd_volt();
  else if (line == "SCAN") cmd_scan();
  else if (line == "LEDS") { led_test(); Serial.println("LEDS ok"); }
  else if (line.startsWith("REC")) cmd_rec((uint32_t)line.substring(3).toInt());
  else if (line.length()) Serial.printf("ERR comando desconhecido: %s\n", line.c_str());
}
