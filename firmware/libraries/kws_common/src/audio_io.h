// audio_io.h — leitura do microfone INMP441 pelo periférico I2S do ESP32.
//
// Fluxo físico:
//   INMP441 --(SD, bit a bit, sincronizado por SCK/WS)--> periférico I2S
//   periférico I2S --(DMA, sem usar a CPU)--> buffers DMA na RAM
//   interrupção "buffer DMA cheio" --> driver acorda quem está em i2s_read()
//   audio_read_block() --> converte 32 bits -> int16, remove DC
#pragma once
#include <Arduino.h>
#include <driver/i2s.h>
#include "kws_config.h"

// Instala o driver I2S (modo mestre, recepção estéreo). Retorna false se falhar.
bool audio_begin();
void audio_end();

// Lê exatamente n amostras (n <= AUDIO_BLOCK_SAMPLES) já convertidas para int16.
// Bloqueia a task chamadora até os dados chegarem ou até `timeout`.
// Retorna o número de amostras efetivamente lidas.
size_t audio_read_block(int16_t *out, size_t n, TickType_t timeout = portMAX_DELAY);

// Lê n quadros estéreos crus: raw[2*i] = slot0, raw[2*i+1] = slot1.
// `raw` precisa ter espaço para 2*n palavras. Retorna quadros lidos.
size_t audio_read_raw(int32_t *raw, size_t n, TickType_t timeout = portMAX_DELAY);

// Descarta o que estiver acumulado nos buffers DMA (áudio "velho").
void audio_flush();

// Converte n quadros estéreos crus em n amostras int16 do slot do microfone.
void audio_convert(const int32_t *raw, int16_t *out, size_t n);
