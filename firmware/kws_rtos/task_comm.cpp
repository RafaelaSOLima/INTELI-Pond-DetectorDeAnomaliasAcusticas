// task_comm.cpp — T4: COMUNICAÇÃO COM O COMPUTADOR (prioridade 2, a mais baixa).
//
// Protocolo: uma linha de texto por mensagem, terminada em '\n', 921600 baud.
//   PC -> ESP32:  TARGET cat | TARGET none | PING <n> | STATS | INJ <bytes> (+ bytes de áudio)
//   ESP32 -> PC:  JSON  {"t":"result",...} {"t":"hb",...} {"t":"pong",...} {"t":"target",...}
// Heartbeat ("hb") a cada 1 s: se o PC parar de recebê-lo, sabe que perdeu a conexão.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

static void send_stats(char *buf, size_t n) {
  snprintf(buf, n,
           "{\"t\":\"hb\",\"ms\":%lu,\"listening\":%d,\"target\":\"%s\",\"blocks\":%lu,\"frames\":%lu,"
           "\"ring_drops\":%lu,\"dma_gaps\":%lu,\"read_errors\":%lu,\"inject_blocks\":%lu,\"seq_gaps\":%lu,\"onsets\":%lu,"
           "\"windows\":%lu,\"win_drops\":%lu,\"results\":%lu,\"feat_us_avg\":%lu,\"feat_us_max\":%lu,"
           "\"inf_us_max\":%lu,\"last_tx_us\":%lu,\"heap\":%u,"
           "\"stack_free\":{\"capture\":%u,\"features\":%u,\"detect\":%u,\"comm\":%u}}",
           (unsigned long)(esp_timer_get_time() / 1000), (xEventGroupGetBits(g_events) & EVT_LISTENING) ? 1 : 0,
           g_target, (unsigned long)g_stats.blocks, (unsigned long)g_stats.frames, (unsigned long)g_stats.ring_drops,
           (unsigned long)g_stats.dma_gaps, (unsigned long)g_stats.read_errors, (unsigned long)g_stats.inject_blocks, (unsigned long)g_stats.seq_gaps,
           (unsigned long)g_stats.onsets, (unsigned long)g_stats.windows, (unsigned long)g_stats.win_drops,
           (unsigned long)g_stats.results, (unsigned long)g_stats.feat_us_avg, (unsigned long)g_stats.feat_us_max,
           (unsigned long)g_stats.inf_us_max, (unsigned long)g_stats.last_tx_us, (unsigned)ESP.getFreeHeap(),
           // "high water mark" = menor espaço livre que a stack já teve (em palavras de 4 bytes)
           (unsigned)uxTaskGetStackHighWaterMark(g_tCapture), (unsigned)uxTaskGetStackHighWaterMark(g_tFeatures),
           (unsigned)uxTaskGetStackHighWaterMark(g_tDetect), (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

// Recebe <total> bytes de áudio int16 da USB e os repassa à T1 pelo stream buffer.
// xStreamBufferSend bloqueia a T4 quando o buffer enche: a T1 esvazia 512 bytes
// a cada 16 ms, então a T4 é "freada" no ritmo real do áudio.
static void receive_injection(long total, char *out, size_t n) {
  static uint8_t chunk[256];
  long got = 0;
  int64_t t_last = esp_timer_get_time();
  while (got < total) {
    int avail = Serial.available();
    if (avail <= 0) {
      if (esp_timer_get_time() - t_last > 1000000) break;  // 1 s sem dados: aborta
      vTaskDelay(1);
      continue;
    }
    long want = total - got;
    if (want > avail) want = avail;
    if (want > (long)sizeof(chunk)) want = sizeof(chunk);
    size_t k = Serial.readBytes(chunk, (size_t)want);
    xStreamBufferSend(g_sbInject, chunk, k, portMAX_DELAY);
    got += (long)k;
    t_last = esp_timer_get_time();
  }
  snprintf(out, n, "{\"t\":\"inj\",\"bytes\":%ld,\"ok\":%s}", got, got == total ? "true" : "false");
}

static void handle_command(char *cmd, char *out, size_t n) {
  if (strncmp(cmd, "TARGET", 6) == 0) {
    const char *w = cmd + 6;
    while (*w == ' ') w++;
    bool none = (*w == 0) || strcmp(w, "none") == 0;
    if (!none && strcmp(w, "ball") && strcmp(w, "cat") && strcmp(w, "dog")) {
      snprintf(out, n, "{\"t\":\"error\",\"msg\":\"alvo invalido: %s\"}", w);
      return;
    }
    xSemaphoreTake(g_mtxGame, portMAX_DELAY);  // T3 lê g_target: escrita protegida
    strncpy(g_target, none ? "" : w, sizeof(g_target) - 1);
    g_target[sizeof(g_target) - 1] = 0;
    g_round++;
    uint32_t r = g_round;
    xSemaphoreGive(g_mtxGame);
    snprintf(out, n, "{\"t\":\"target\",\"target\":\"%s\",\"round\":%lu}", none ? "" : w, (unsigned long)r);
  } else if (strncmp(cmd, "PING", 4) == 0) {
    // O PC mede o tempo de ida e volta (latência de comunicação) com esta resposta.
    snprintf(out, n, "{\"t\":\"pong\",\"n\":\"%s\",\"esp_us\":%lld}", cmd[4] ? cmd + 5 : "", (long long)esp_timer_get_time());
  } else if (strncmp(cmd, "INJ ", 4) == 0) {
    receive_injection(atol(cmd + 4), out, n);
  } else if (strcmp(cmd, "STATS") == 0) {
    send_stats(out, n);
  } else if (cmd[0]) {
    snprintf(out, n, "{\"t\":\"error\",\"msg\":\"comando desconhecido\"}");
  }
}

void task_comm(void *arg) {
  (void)arg;
  static char rx[64];
  static char out[640];
  size_t len = 0;
  TickType_t last_hb = xTaskGetTickCount();

  for (;;) {
    // Lê o que chegou pela USB (não bloqueante) e monta linhas.
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\r') continue;
      if (c == '\n') {
        rx[len] = 0;
        out[0] = 0;
        handle_command(rx, out, sizeof(out));
        if (out[0]) serial_line(out);
        len = 0;
      } else if (len < sizeof(rx) - 1) {
        rx[len++] = c;
      }
    }
    if (xTaskGetTickCount() - last_hb >= pdMS_TO_TICKS(1000)) {
      last_hb = xTaskGetTickCount();
      send_stats(out, sizeof(out));
      serial_line(out);
    }
    vTaskDelay(pdMS_TO_TICKS(10));  // cede a CPU: checa a USB 100x por segundo
  }
}
