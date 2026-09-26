// app.h — tipos, objetos do FreeRTOS e estado compartilhados pelas 4 tasks.
//
// Quem cria cada objeto: setup() em kws_rtos.ino, ANTES de criar as tasks
// (assim nenhuma task usa um handle ainda nulo).
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include "kws_config.h"
#include "kws_features.h"
#include "kws_model.h"

// ------------------------------------------------------------------ prioridades
// Maior número = maior prioridade. Todas no mesmo núcleo (APP_CORE) para que a
// preempção por prioridade aconteça de verdade entre elas.
#define APP_CORE        1
#define PRIO_CAPTURE    5   // T1: não pode atrasar (perderia áudio)
#define PRIO_FEATURES   4   // T2: tem que acompanhar o ritmo da captura (1 frame / 16 ms)
#define PRIO_DETECT     3   // T3: roda só quando há uma palavra (pode esperar alguns ms)
#define PRIO_COMM       2   // T4: comandos do PC e heartbeat (o menos urgente)

#define STACK_CAPTURE   3072
#define STACK_FEATURES  6144
#define STACK_DETECT    4096
#define STACK_COMM      5120

// ------------------------------------------------------------------ buffer circular (T1 -> T2)
// RING_BLOCKS blocos de 256 amostras. A fila de índices tem RING_BLOCKS-3 posições:
// assim, mesmo com a fila cheia e a T2 processando um bloco, a T1 nunca
// sobrescreve um bloco que a T2 ainda vai ler (ver docs/freertos.md).
#define RING_BLOCKS     19                       // 19 x 16 ms = 304 ms de folga
#define QBLOCKS_LEN     (RING_BLOCKS - 3)

typedef struct {
  uint16_t slot;       // índice do bloco no buffer circular
  uint32_t seq;        // número sequencial do bloco (detecta perdas)
  int64_t t_ready_us;  // quando a T1 terminou de ler/converter o bloco
  uint32_t read_us;    // quanto tempo a T1 levou para converter o bloco (latência de captura)
} BlockMsg;

extern int16_t g_ring[RING_BLOCKS][AUDIO_BLOCK_SAMPLES];

// ------------------------------------------------------------------ janela (T2 -> T3)
typedef struct {
  float feats[KWS_WIN_FRAMES * KWS_N_FEAT];  // 61 x 15 features (copiadas para a fila)
  uint32_t seq;           // número da janela
  int64_t t_onset_us;     // quando o VAD detectou o início da fala
  int64_t t_last_block_us;// quando o último bloco de áudio da janela ficou pronto (T1)
  int64_t t_sent_us;      // quando a T2 colocou a janela na fila
  uint32_t capture_us;    // conversão do último bloco na T1
  uint32_t queue_us;      // espera do último bloco na fila T1->T2
  uint32_t feat_us;       // tempo de cálculo das features do último frame
} WindowMsg;

#define QWINDOWS_LEN    2

// ------------------------------------------------------------------ injeção de áudio (teste)
// O script tests/perf_test.py envia WAVs pela USB; a T4 coloca os bytes neste
// stream buffer e a T1 usa esses blocos NO LUGAR do microfone. Todo o resto do
// pipeline (T2, T3, LED) é exatamente o mesmo do uso real.
#define INJECT_SB_BYTES 8192

// ------------------------------------------------------------------ Event Group
#define EVT_LISTENING   (1 << 0)   // 1 = aceita uma nova palavra; 0 = mostrando resultado

// ------------------------------------------------------------------ handles globais
extern QueueHandle_t g_qBlocks;
extern QueueHandle_t g_qWindows;
extern SemaphoreHandle_t g_mtxSerial;
extern SemaphoreHandle_t g_mtxGame;
extern EventGroupHandle_t g_events;
extern StreamBufferHandle_t g_sbInject;
extern TaskHandle_t g_tCapture, g_tFeatures, g_tDetect, g_tComm;

// ------------------------------------------------------------------ estado do jogo (protegido por g_mtxGame)
extern char g_target[12];   // palavra-alvo ("" = modo livre)
extern uint32_t g_round;

// ------------------------------------------------------------------ estatísticas
// Cada contador tem UM único escritor (a task indicada) e é uma palavra de 32
// bits alinhada: no ESP32 a escrita é atômica, então a T4 pode lê-los sem mutex
// (no pior caso lê o valor de um instante antes). Por isso não usamos mutex aqui.
typedef struct {
  volatile uint32_t blocks;       // T1: blocos lidos
  volatile uint32_t ring_drops;   // T1: blocos descartados (fila cheia = T2 atrasada)
  volatile uint32_t dma_gaps;     // T1: intervalos > folga do DMA (possível perda no hardware)
  volatile uint32_t read_errors;  // T1: leituras incompletas do I2S
  volatile uint32_t inject_blocks;// T1: blocos que vieram da injeção (teste) em vez do microfone
  volatile uint32_t frames;       // T2: frames processados
  volatile uint32_t feat_us_max;  // T2: pior tempo de um frame
  volatile uint32_t feat_us_avg;  // T2: média móvel do tempo de um frame
  volatile uint32_t seq_gaps;     // T2: blocos que faltaram na sequência
  volatile uint32_t onsets;       // T2: inícios de fala detectados
  volatile uint32_t windows;      // T2: janelas enviadas à T3
  volatile uint32_t win_drops;    // T2: janelas descartadas (fila da T3 cheia)
  volatile uint32_t results;      // T3: classificações feitas
  volatile uint32_t last_tx_us;   // T3: tempo para escrever o último resultado na USB
  volatile uint32_t inf_us_max;   // T3: pior tempo de inferência
} Stats;

extern Stats g_stats;

// ------------------------------------------------------------------ funções das tasks
void task_capture(void *arg);
void task_features(void *arg);
void task_detect(void *arg);
void task_comm(void *arg);

// Escreve uma linha na Serial com o mutex (bloqueia até 50 ms esperando o mutex).
void serial_line(const char *line);
