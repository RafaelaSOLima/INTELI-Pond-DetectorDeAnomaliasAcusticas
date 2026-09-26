// task_features.cpp — T2: EXTRAÇÃO DE FEATURES (prioridade 4).
//
// Para cada bloco novo (16 ms) vindo da fila qBlocks:
//   1) monta um frame de 512 amostras = bloco anterior + bloco atual
//      (frames sobrepostos em 50%, como no treino);
//   2) calcula 15 features (13 MFCC, log RMS, centroid) -> histórico circular;
//   3) atualiza o detector de fala (VAD) com o log RMS;
//   4) se o sistema está ouvindo (bit EVT_LISTENING) e a fala começou, espera
//      completar 61 frames a partir de 8 frames ANTES do início e envia uma
//      CÓPIA da janela para a T3 pela fila qWindows.
#include <string.h>

#include "app.h"

#define HIST_FRAMES 128  // histórico de features (2 s) — maior que PRE_ROLL + 61

static float s_hist[HIST_FRAMES][KWS_N_FEAT];
static int16_t s_frame[KWS_FRAME_LEN];
static WindowMsg s_win;  // 3,7 KB: estático para não ocupar a stack da task

void task_features(void *arg) {
  (void)arg;
  kws_vad_t vad;
  kws_vad_reset(&vad);
  bool have_prev = false;
  uint32_t expected_seq = 0;
  uint32_t frame_idx = 0;         // índice absoluto do próximo frame
  bool pending = false;           // há uma janela sendo completada?
  uint32_t win_start = 0;         // frame inicial da janela pendente
  int64_t t_onset = 0;
  uint32_t win_seq = 0;

  for (;;) {
    BlockMsg msg;
    // Bloqueia (sem gastar CPU) até a T1 publicar um bloco.
    xQueueReceive(g_qBlocks, &msg, portMAX_DELAY);
    int64_t t_rx = esp_timer_get_time();

    if (msg.seq != expected_seq) {  // blocos perdidos: o frame "atravessaria" um buraco
      g_stats.seq_gaps += msg.seq - expected_seq;
      have_prev = false;
    }
    expected_seq = msg.seq + 1;

    // Frame = [bloco anterior | bloco atual]. A metade "anterior" já está em
    // s_frame[256..511] da iteração passada: desliza e copia o bloco novo.
    // Copiamos do buffer circular logo ao receber: depois disso a T1 pode
    // reutilizar o slot sem problema.
    memmove(s_frame, s_frame + KWS_HOP, KWS_HOP * sizeof(int16_t));
    memcpy(s_frame + KWS_HOP, g_ring[msg.slot], KWS_HOP * sizeof(int16_t));
    if (!have_prev) {
      have_prev = true;
      continue;  // precisa de 2 blocos para o primeiro frame
    }

    int64_t t0 = esp_timer_get_time();
    float *f = s_hist[frame_idx % HIST_FRAMES];
    kws_features_frame(s_frame, f);
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    if (dt > g_stats.feat_us_max) g_stats.feat_us_max = dt;
    g_stats.feat_us_avg = (g_stats.feat_us_avg * 31 + dt) / 32;  // média móvel
    g_stats.frames++;

    int onset = kws_vad_update(&vad, f[13]);
    bool listening = (xEventGroupGetBits(g_events) & EVT_LISTENING) != 0;

    if (onset) g_stats.onsets++;
    if (onset && listening && !pending) {
      pending = true;
      win_start = (frame_idx >= KWS_PRE_ROLL_FRAMES) ? frame_idx - KWS_PRE_ROLL_FRAMES : 0;
      t_onset = t0;
    }

    if (pending && frame_idx == win_start + KWS_WIN_FRAMES - 1) {
      // Janela completa: copia os 61 frames do histórico circular, em ordem.
      for (uint32_t k = 0; k < KWS_WIN_FRAMES; k++)
        memcpy(&s_win.feats[k * KWS_N_FEAT], s_hist[(win_start + k) % HIST_FRAMES], sizeof(float) * KWS_N_FEAT);
      s_win.seq = win_seq++;
      s_win.t_onset_us = t_onset;
      s_win.t_last_block_us = msg.t_ready_us;
      s_win.capture_us = msg.read_us;
      s_win.queue_us = (uint32_t)(t_rx - msg.t_ready_us);
      s_win.feat_us = dt;
      s_win.t_sent_us = esp_timer_get_time();
      // Para de aceitar palavras até a T3 terminar de mostrar o resultado.
      xEventGroupClearBits(g_events, EVT_LISTENING);
      // xQueueSend COPIA os 3,7 KB para dentro da fila: a T3 recebe uma cópia
      // própria e a T2 pode seguir escrevendo em s_hist sem conflito.
      if (xQueueSend(g_qWindows, &s_win, 0) != pdTRUE) {
        g_stats.win_drops++;
        xEventGroupSetBits(g_events, EVT_LISTENING);
      } else {
        g_stats.windows++;
      }
      pending = false;
    }
    frame_idx++;
  }
}
