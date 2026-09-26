// kws_features.h — extração de features de áudio (C puro, sem Arduino).
//
// Este arquivo é compilado em DOIS lugares:
//   1) no ESP32 (Task 2 — Feature Extraction);
//   2) no computador (gcc), pelo teste tests/test_features.py, que compara o
//      resultado com training/features.py (a versão Python usada no treino).
// Se C e Python divergirem, o modelo treinado no PC erra no ESP32 — por isso
// as duas implementações seguem exatamente as mesmas fórmulas e constantes.
//
// Por frame (512 amostras = 32 ms, avançando 256 amostras = 16 ms):
//   feat[0..12]  13 MFCCs      — "formato" do espectro (qual som foi falado)
//   feat[13]     log10(RMS)    — energia do frame (também usada no detector de fala)
//   feat[14]     centroid/8000 — "centro de massa" do espectro, 0..1 (brilho)
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_SAMPLE_RATE   16000
#define KWS_FRAME_LEN     512      // amostras por frame (32 ms) = tamanho da FFT
#define KWS_HOP           256      // avanço entre frames (16 ms) = tamanho do bloco da captura
#define KWS_N_BINS        (KWS_FRAME_LEN / 2 + 1)  // 257 bins de frequência (0..8 kHz)
#define KWS_N_MEL         40       // filtros triangulares na escala mel
#define KWS_N_MFCC        13
#define KWS_N_FEAT        15       // 13 MFCC + log RMS + centroid
#define KWS_MEL_FMIN      20.0f
#define KWS_MEL_FMAX      8000.0f
#define KWS_LOG_EPS       1e-6f

// Janela de classificação: 61 frames = 16 ms * 60 + 32 ms = 992 ms de áudio.
#define KWS_WIN_FRAMES    61

// Detector de início de fala (VAD por energia) — ver kws_vad_update().
#define KWS_VAD_ON_DB        10.0f   // fala = energia > piso de ruído + 10 dB
#define KWS_VAD_MIN_DB      -60.0f   // e acima de -60 dBFS (ignora "silêncio absoluto")
#define KWS_VAD_ON_FRAMES    2       // por 2 frames seguidos (32 ms) — evita estalos
#define KWS_VAD_FLOOR_UP     0.005f  // piso sobe devagar (~3 s)...
#define KWS_VAD_FLOOR_DOWN   0.2f    // ...e desce rápido
#define KWS_VAD_MAX_FRAMES   125     // após 2 s seguidos de "fala", o piso volta a subir (ruído permanente)
#define KWS_PRE_ROLL_FRAMES  8       // o pico é procurado a partir de 8 frames antes do início detectado
// Alinhamento da janela (igual no treino e no ESP32): depois que o VAD dispara,
// procura o frame de MAIOR ENERGIA (núcleo da palavra) nos próximos
// KWS_PEAK_SEARCH_FRAMES frames e posiciona a janela com esse pico no frame
// KWS_PEAK_POS. Assim um disparo precoce (ruído logo antes da fala) não desalinha.
#define KWS_PEAK_SEARCH_FRAMES 45    // 720 ms de busca após o disparo
#define KWS_PEAK_POS           15    // posição do pico dentro da janela de 61 frames

// Prepara tabelas (janela de Hann, FFT, banco mel, DCT). Chamar uma vez.
void kws_features_init(void);

// Calcula as KWS_N_FEAT features de UM frame de KWS_FRAME_LEN amostras int16.
// Usa buffers internos estáticos: só UMA task pode chamar (no ESP32, a Task 2).
void kws_features_frame(const int16_t *frame, float *feat_out);

// Estado do detector de fala. Zere com kws_vad_reset().
typedef struct {
  float floor_db;  // estimativa do piso de ruído (dBFS)
  int above;       // frames consecutivos acima do limiar
  int init;        // 0 até o primeiro frame
} kws_vad_t;

void kws_vad_reset(kws_vad_t *v);
// Recebe log10(RMS) do frame (feat[13]). Retorna 1 no frame em que a fala COMEÇA
// (exatamente uma vez por "subida"), 0 caso contrário.
int kws_vad_update(kws_vad_t *v, float log_rms);

#ifdef __cplusplus
}
#endif
