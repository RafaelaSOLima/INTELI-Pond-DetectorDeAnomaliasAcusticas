// kws_rtos.ino — FIRMWARE PRINCIPAL: reconhecimento de "ball", "cat", "dog" no ESP32.
//
// Todo o processamento acústico roda aqui (edge): captura, features, modelo,
// decisão e LED. O computador só mostra a interface e escolhe a palavra-alvo.
//
// Tasks (todas no núcleo 1):
//   T1 task_capture   prio 5  I2S -> buffer circular -> fila qBlocks
//   T2 task_features  prio 4  qBlocks -> RMS/centroid/MFCC + VAD -> fila qWindows
//   T3 task_detect    prio 3  qWindows -> CNN -> decisão -> LED + JSON
//   T4 task_comm      prio 2  comandos do PC (TARGET/PING) + heartbeat
#include "app.h"
#include "audio_io.h"

// Definição dos objetos declarados em app.h
int16_t g_ring[RING_BLOCKS][AUDIO_BLOCK_SAMPLES];
QueueHandle_t g_qBlocks;
QueueHandle_t g_qWindows;
SemaphoreHandle_t g_mtxSerial;
SemaphoreHandle_t g_mtxGame;
EventGroupHandle_t g_events;
StreamBufferHandle_t g_sbInject;
TaskHandle_t g_tCapture, g_tFeatures, g_tDetect, g_tComm;
char g_target[12] = "";
uint32_t g_round = 0;
Stats g_stats;

void serial_line(const char *line) {
  // Sem este mutex, a T3 (resultado) e a T4 (heartbeat) poderiam intercalar
  // caracteres e o PC receberia um JSON quebrado.
  if (xSemaphoreTake(g_mtxSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
    Serial.print(line);
    Serial.print('\n');
    xSemaphoreGive(g_mtxSerial);
  }
}

static void fatal(const char *msg) {
  Serial.printf("{\"t\":\"error\",\"msg\":\"%s\"}\n", msg);
  pinMode(PIN_LED, OUTPUT);
  for (;;) {  // LED piscando rápido para sempre = erro de inicialização
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    delay(100);
  }
}

void setup() {
  Serial.setRxBufferSize(16384);  // espaço para o áudio injetado pelo teste
  Serial.begin(921600);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  delay(200);

  kws_features_init();  // tabelas da FFT/mel/DCT (antes das tasks)
  if (!audio_begin()) fatal("falha ao iniciar I2S");

  // 1) Cria filas, mutexes e event group ANTES das tasks que os usam.
  g_qBlocks = xQueueCreate(QBLOCKS_LEN, sizeof(BlockMsg));
  g_qWindows = xQueueCreate(QWINDOWS_LEN, sizeof(WindowMsg));
  g_mtxSerial = xSemaphoreCreateMutex();
  g_mtxGame = xSemaphoreCreateMutex();
  g_events = xEventGroupCreate();
  g_sbInject = xStreamBufferCreate(INJECT_SB_BYTES, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
  if (!g_qBlocks || !g_qWindows || !g_mtxSerial || !g_mtxGame || !g_events || !g_sbInject)
    fatal("sem memoria para objetos RTOS");
  xEventGroupSetBits(g_events, EVT_LISTENING);  // começa ouvindo (modo livre)

  Serial.printf("{\"t\":\"boot\",\"model\":\"%s\",\"placeholder\":%d,\"threshold\":%.2f,"
                "\"classes\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"],\"heap\":%u}\n",
                KWS_MODEL_SHA, KWS_MODEL_IS_PLACEHOLDER, (double)KWS_MODEL_THRESHOLD,
                KWS_MODEL_CLASSES[0], KWS_MODEL_CLASSES[1], KWS_MODEL_CLASSES[2], KWS_MODEL_CLASSES[3],
                KWS_MODEL_CLASSES[4], (unsigned)ESP.getFreeHeap());

  // 2) Cria as tasks, todas fixadas no núcleo APP_CORE.
  //    Ordem: consumidores primeiro, para que já estejam esperando nas filas.
  xTaskCreatePinnedToCore(task_comm, "comm", STACK_COMM, NULL, PRIO_COMM, &g_tComm, APP_CORE);
  xTaskCreatePinnedToCore(task_detect, "detect", STACK_DETECT, NULL, PRIO_DETECT, &g_tDetect, APP_CORE);
  xTaskCreatePinnedToCore(task_features, "features", STACK_FEATURES, NULL, PRIO_FEATURES, &g_tFeatures, APP_CORE);
  xTaskCreatePinnedToCore(task_capture, "capture", STACK_CAPTURE, NULL, PRIO_CAPTURE, &g_tCapture, APP_CORE);
}

void loop() {
  // O Arduino roda loop() numa task própria ("loopTask"). Não precisamos dela:
  // tudo acontece nas 4 tasks acima. Ela se apaga e libera a memória.
  vTaskDelete(NULL);
}
