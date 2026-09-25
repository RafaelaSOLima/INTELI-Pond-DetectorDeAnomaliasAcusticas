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

// Um único LED de alerta (o LED verde queimou na montagem).
//   acerto          -> 1 piscada longa  (LED_OK_MS aceso)
//   erro / unknown  -> 3 piscadas curtas (LED_FAIL_ON_MS aceso, LED_FAIL_OFF_MS apagado)
#define PIN_LED       19   // GPIO -> resistor -> LED (anodo) -> GND
#define LED_OK_MS        1000
#define LED_FAIL_ON_MS    120
#define LED_FAIL_OFF_MS   120

// ---------------------------------------------------------------------------
// Áudio
// ---------------------------------------------------------------------------
#define AUDIO_SAMPLE_RATE   16000  // Hz — fala útil vai até ~8 kHz (Nyquist)
#define AUDIO_BLOCK_SAMPLES 256    // amostras por bloco = 16 ms (= hop das features)

// DMA do I2S: 8 buffers x 256 quadros (frames) = 128 ms de folga.
// Cada quadro estéreo tem 2 palavras de 32 bits (slot esquerdo + direito) => 16 KB.
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

// Slot do microfone dentro do quadro estéreo.
// Lemos SEMPRE em estéreo (os dois slots) e escolhemos o slot do microfone.
// Motivo: no driver I2S legado do ESP32 (core 2.0.x) com 32 bits, os modos
// mono ONLY_LEFT/ONLY_RIGHT não entregaram o slot correto com L/R no GND
// (medido no hardware). Com L/R -> GND, o comando CHAN mostrou:
//   slot0: sem sinal   |   slot1: sinal do microfone
// Se o CHAN algum dia indicar o outro slot, troque este valor.
#define AUDIO_MIC_SLOT      1

// Duração do clipe gravado para o dataset (o treino recorta 1 s dentro dele).
#define REC_CLIP_MS         1500
