// task_detect.cpp — T3: DETECÇÃO / CLASSIFICAÇÃO (prioridade 3).
//
//   1) espera uma janela na fila qWindows (bloqueada, sem gastar CPU);
//   2) roda a CNN (kws_model_run) -> 5 probabilidades;
//   3) decide: palavra-alvo só se for ball/cat/dog COM confiança >= threshold,
//      senão "unknown" (inclui a classe noise);
//   4) compara com a palavra-alvo (lida com o mutex g_mtxGame);
//   5) acende o LED (única task que mexe no LED -> sem conflito);
//   6) envia o resultado em JSON (com o mutex g_mtxSerial);
//   7) espera o "cooldown" e liga de novo o bit EVT_LISTENING.
#include <stdio.h>
#include <string.h>

#include "app.h"

#define COOLDOWN_MS 600  // após o LED, ignora sons por mais este tempo (eco da própria fala)

static WindowMsg s_win;  // cópia recebida da fila (estática: fora da stack)

static bool is_target(int k) { return k <= 2; }  // classes 0..2 = ball, cat, dog

// Padrões do LED. vTaskDelay() BLOQUEIA SÓ ESTA TASK e libera a CPU: enquanto o
// LED pisca, T1 e T2 continuam capturando e processando normalmente.
static void led_ok() {
  digitalWrite(PIN_LED, HIGH);
  vTaskDelay(pdMS_TO_TICKS(LED_OK_MS));
  digitalWrite(PIN_LED, LOW);
}
static void led_fail() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED, HIGH);
    vTaskDelay(pdMS_TO_TICKS(LED_FAIL_ON_MS));
    digitalWrite(PIN_LED, LOW);
    vTaskDelay(pdMS_TO_TICKS(LED_FAIL_OFF_MS));
  }
}

void task_detect(void *arg) {
  (void)arg;
  static char line[512];
  for (;;) {
    xQueueReceive(g_qWindows, &s_win, portMAX_DELAY);
    int64_t t_rx = esp_timer_get_time();

    // ---------------- inferência
    float probs[KWS_MODEL_N_CLASSES];
    int64_t t_inf0 = esp_timer_get_time();
    kws_model_run(s_win.feats, probs);
    int64_t t_inf1 = esp_timer_get_time();
    uint32_t inf_us = (uint32_t)(t_inf1 - t_inf0);
    if (inf_us > g_stats.inf_us_max) g_stats.inf_us_max = inf_us;

    // ---------------- decisão
    int best = 0;
    for (int k = 1; k < KWS_MODEL_N_CLASSES; k++)
      if (probs[k] > probs[best]) best = k;
    const char *word = (is_target(best) && probs[best] >= KWS_MODEL_THRESHOLD) ? KWS_MODEL_CLASSES[best] : "unknown";

    char target[sizeof(g_target)];
    uint32_t round;
    xSemaphoreTake(g_mtxGame, portMAX_DELAY);  // seção crítica curta: só copia
    strcpy(target, g_target);
    round = g_round;
    xSemaphoreGive(g_mtxGame);

    bool known = strcmp(word, "unknown") != 0;
    bool ok = target[0] ? (strcmp(word, target) == 0) : known;  // sem alvo: qualquer palavra conhecida vale

    // ---------------- LED (o instante em que o LED acende fecha a latência total)
    int64_t t_led = esp_timer_get_time();
    digitalWrite(PIN_LED, HIGH);

    // ---------------- resultado para o PC
    snprintf(line, sizeof(line),
             "{\"t\":\"result\",\"seq\":%lu,\"round\":%lu,\"word\":\"%s\",\"best\":\"%s\",\"conf\":%.3f,"
             "\"probs\":[%.3f,%.3f,%.3f,%.3f,%.3f],\"target\":\"%s\",\"ok\":%s,"
             "\"lat_us\":{\"capture\":%lu,\"queue\":%lu,\"features\":%lu,\"to_detect\":%lu,\"inference\":%lu,"
             "\"end_to_led\":%lu,\"onset_to_led\":%lu,\"prev_tx\":%lu}}",
             (unsigned long)s_win.seq, (unsigned long)round, word, KWS_MODEL_CLASSES[best], (double)probs[best],
             (double)probs[0], (double)probs[1], (double)probs[2], (double)probs[3], (double)probs[4], target,
             ok ? "true" : "false", (unsigned long)s_win.capture_us, (unsigned long)s_win.queue_us,
             (unsigned long)s_win.feat_us, (unsigned long)(t_rx - s_win.t_sent_us), (unsigned long)inf_us,
             (unsigned long)(t_led - s_win.t_last_block_us), (unsigned long)(t_led - s_win.t_onset_us),
             (unsigned long)g_stats.last_tx_us);
    int64_t t_tx0 = esp_timer_get_time();
    serial_line(line);
    g_stats.last_tx_us = (uint32_t)(esp_timer_get_time() - t_tx0);  // latência de comunicação (envio)
    g_stats.results++;

    // ---------------- padrão do LED + cooldown
    digitalWrite(PIN_LED, LOW);
    if (ok) led_ok();
    else led_fail();
    vTaskDelay(pdMS_TO_TICKS(COOLDOWN_MS));
    xEventGroupSetBits(g_events, EVT_LISTENING);  // pronto para a próxima palavra
  }
}
