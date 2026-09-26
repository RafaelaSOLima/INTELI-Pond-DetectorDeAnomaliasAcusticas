// task_capture.cpp — T1: CAPTURA CONTÍNUA DE ÁUDIO (prioridade 5, a mais alta).
//
// Laço infinito:
//   1) audio_read_raw(): a task BLOQUEIA dentro de i2s_read() até o DMA
//      completar um buffer. Quem a acorda é a interrupção do I2S. Enquanto
//      espera, ela não gasta CPU — as outras tasks rodam.
//   2) grava o bloco (256 amostras = 16 ms) no próximo slot do buffer circular;
//   3) avisa a T2 colocando o ÍNDICE do slot na fila qBlocks, com timeout 0:
//      se a fila estiver cheia (T2 atrasada), descarta e conta — a captura
//      NUNCA espera por quem está depois dela.
#include "app.h"
#include "audio_io.h"

// Folga total do DMA: se passar mais que isso entre duas leituras, o hardware
// pode ter sobrescrito áudio (I2S_DMA_BUF_COUNT * I2S_DMA_BUF_LEN amostras).
static const int64_t kDmaSlackUs = (int64_t)I2S_DMA_BUF_COUNT * I2S_DMA_BUF_LEN * 1000000LL / AUDIO_SAMPLE_RATE;

void task_capture(void *arg) {
  (void)arg;
  uint16_t slot = 0;
  uint32_t seq = 0;
  int64_t t_prev = esp_timer_get_time();

  static int32_t raw[2 * AUDIO_BLOCK_SAMPLES];  // quadros estéreos crus do I2S (4 KB, fora da stack)

  for (;;) {
    // (a) Espera bloqueante pelo DMA (timeout de 200 ms só para detectar I2S parado).
    size_t n = audio_read_raw(raw, AUDIO_BLOCK_SAMPLES, pdMS_TO_TICKS(200));
    int64_t t_dma = esp_timer_get_time();
    if (n != AUDIO_BLOCK_SAMPLES) {
      g_stats.read_errors++;
      continue;
    }
    if (t_dma - t_prev > kDmaSlackUs) g_stats.dma_gaps++;
    t_prev = t_dma;
    // (b) Conversão 32 bits -> int16 + DC blocker, direto no slot do buffer circular.
    audio_convert(raw, g_ring[slot], AUDIO_BLOCK_SAMPLES);
    // (b') Modo de teste: se há áudio injetado pelo PC, ele substitui o bloco do
    //      microfone. O ritmo continua sendo o do I2S (1 bloco a cada 16 ms).
    const size_t kBlockBytes = AUDIO_BLOCK_SAMPLES * sizeof(int16_t);
    if (xStreamBufferBytesAvailable(g_sbInject) >= kBlockBytes) {
      xStreamBufferReceive(g_sbInject, g_ring[slot], kBlockBytes, 0);
      g_stats.inject_blocks++;
    }
    int64_t t_ready = esp_timer_get_time();

    BlockMsg msg;
    msg.slot = slot;
    msg.seq = seq++;
    msg.t_ready_us = t_ready;
    msg.read_us = (uint32_t)(t_ready - t_dma);  // latência de CAPTURA (processamento da T1)
    if (xQueueSend(g_qBlocks, &msg, 0) != pdTRUE) g_stats.ring_drops++;
    g_stats.blocks++;
    slot = (slot + 1) % RING_BLOCKS;  // buffer CIRCULAR: depois do último, volta ao primeiro
  }
}
