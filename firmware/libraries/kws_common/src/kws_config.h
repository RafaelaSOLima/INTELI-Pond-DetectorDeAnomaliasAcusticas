// kws_config.h — constantes de hardware e de áudio compartilhadas.
//
// Tudo que precisa ser IGUAL entre o gravador do dataset, o firmware principal
// e o código Python de treino fica aqui. Se mudar algo de áudio aqui depois de
// gravar o dataset, o modelo precisa ser retreinado.
#pragma once

// ---------------------------------------------------------------------------
// Pinagem (numeração de GPIO do chip ESP32 — vale para qualquer placa com
// módulo ESP32-WROOM-32 / WROOM-32U). Ver docs/hardware.md.
// ---------------------------------------------------------------------------
#define PIN_I2S_SCK   26   // INMP441 SCK  (bit clock, gerado pelo ESP32)
#define PIN_I2S_WS    25   // INMP441 WS   (word select / LRCLK, gerado pelo ESP32)
#define PIN_I2S_SD    33   // INMP441 SD   (dados seriais, saída do microfone)
// INMP441 L/R -> GND  => microfone responde no canal ESQUERDO (WS = 0)

#define PIN_LED_GREEN 18   // GPIO -> resistor -> LED verde (anodo) -> GND
#define PIN_LED_RED   19   // GPIO -> resistor -> LED vermelho (anodo) -> GND

// ---------------------------------------------------------------------------
// Áudio
// ---------------------------------------------------------------------------
#define AUDIO_SAMPLE_RATE   16000  // Hz — fala útil vai até ~8 kHz (Nyquist)
#define AUDIO_BLOCK_SAMPLES 256    // amostras por bloco = 16 ms (= hop das features)

// DMA do I2S: 8 buffers x 256 amostras x 4 bytes = 8 KB = 128 ms de folga.
// Se a task de captura atrasar menos que isso, nenhuma amostra se perde.
#define I2S_DMA_BUF_COUNT   8
#define I2S_DMA_BUF_LEN     256

// O INMP441 entrega 24 bits úteis dentro de palavras de 32 bits.
// Convertemos para int16 com ganho digital fixo:
//   24 bits -> 16 bits "sem ganho" seria >> 8. Usamos >> AUDIO_SHIFT_24_TO_16.
//   AUDIO_SHIFT_24_TO_16 = 5  =>  ganho de 2^(8-5) = 8x  (~ +18 dB)
// Calibrado para fala a 10–80 cm. NÃO mude depois de gravar o dataset.
#define AUDIO_SHIFT_24_TO_16 5

// Filtro "DC blocker": remove o nível contínuo (offset) do microfone.
//   y[n] = x[n] - x[n-1] + R * y[n-1]
#define AUDIO_DC_BLOCK_R    0.995f

// Canal do microfone (L/R ligado ao GND = esquerdo). Se o teste "CHAN" do
// gravador mostrar sinal só no outro canal, troque para I2S_CHANNEL_FMT_ONLY_RIGHT.
#define AUDIO_CHANNEL_FMT   I2S_CHANNEL_FMT_ONLY_LEFT

// Duração do clipe gravado para o dataset (o treino recorta 1 s dentro dele).
#define REC_CLIP_MS         1500
