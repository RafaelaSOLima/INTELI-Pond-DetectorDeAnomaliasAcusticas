// kws_features.c — ver kws_features.h. Espelho exato de training/features.py.
#include "kws_features.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

// ------------------------------------------------------------------ tabelas
static float s_window[KWS_FRAME_LEN];            // janela de Hann
static float s_cos[KWS_FRAME_LEN / 2];           // twiddles da FFT
static float s_sin[KWS_FRAME_LEN / 2];
static uint16_t s_bitrev[KWS_FRAME_LEN];
static float s_dct[KWS_N_MFCC][KWS_N_MEL];       // matriz DCT-II ortonormal
// Banco mel esparso: cada filtro m cobre os bins [s_mel_start[m], s_mel_start[m]+s_mel_len[m])
static uint16_t s_mel_start[KWS_N_MEL];
static uint16_t s_mel_len[KWS_N_MEL];
static float s_mel_w[2 * KWS_N_BINS];            // pesos concatenados (cada bin cai em <= 2 filtros)
static uint16_t s_mel_off[KWS_N_MEL];

// ------------------------------------------------------------------ buffers de trabalho
static float s_re[KWS_FRAME_LEN];
static float s_im[KWS_FRAME_LEN];
static float s_pow[KWS_N_BINS];

static float hz_to_mel(float hz) { return 2595.0f * log10f(1.0f + hz / 700.0f); }
static float mel_to_hz(float mel) { return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f); }

void kws_features_init(void) {
  const int N = KWS_FRAME_LEN;
  // Hann "periódica": w[n] = 0.5 - 0.5 cos(2*pi*n/N)
  for (int n = 0; n < N; n++) s_window[n] = 0.5f - 0.5f * cosf(2.0f * PI_F * n / N);
  for (int k = 0; k < N / 2; k++) {
    s_cos[k] = cosf(2.0f * PI_F * k / N);
    s_sin[k] = -sinf(2.0f * PI_F * k / N);
  }
  int bits = 0;
  while ((1 << bits) < N) bits++;
  for (int i = 0; i < N; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
    s_bitrev[i] = (uint16_t)r;
  }

  // Banco de filtros mel (HTK): KWS_N_MEL+2 pontos igualmente espaçados em mel.
  float mel_lo = hz_to_mel(KWS_MEL_FMIN), mel_hi = hz_to_mel(KWS_MEL_FMAX);
  float hz_pts[KWS_N_MEL + 2];
  for (int i = 0; i < KWS_N_MEL + 2; i++)
    hz_pts[i] = mel_to_hz(mel_lo + (mel_hi - mel_lo) * i / (KWS_N_MEL + 1));
  const float bin_hz = (float)KWS_SAMPLE_RATE / N;
  uint16_t off = 0;
  for (int m = 0; m < KWS_N_MEL; m++) {
    float lo = hz_pts[m], ce = hz_pts[m + 1], hi = hz_pts[m + 2];
    int first = -1, count = 0;
    s_mel_off[m] = off;
    for (int k = 0; k < KWS_N_BINS; k++) {
      float f = k * bin_hz, w = 0.0f;
      if (f > lo && f < ce) w = (f - lo) / (ce - lo);
      else if (f >= ce && f < hi) w = (hi - f) / (hi - ce);
      if (w > 0.0f) {
        if (first < 0) first = k;
        s_mel_w[off + count] = w;
        count++;
      }
    }
    s_mel_start[m] = (uint16_t)(first < 0 ? 0 : first);
    s_mel_len[m] = (uint16_t)count;
    off += count;
  }

  // DCT-II ortonormal: c[i] = a_i * sum_m logmel[m] * cos(pi*i*(m+0.5)/M)
  for (int i = 0; i < KWS_N_MFCC; i++) {
    float a = (i == 0) ? sqrtf(1.0f / KWS_N_MEL) : sqrtf(2.0f / KWS_N_MEL);
    for (int m = 0; m < KWS_N_MEL; m++)
      s_dct[i][m] = a * cosf(PI_F * i * (m + 0.5f) / KWS_N_MEL);
  }
}

// FFT complexa radix-2 in-place (Cooley-Tukey iterativa).
static void fft(float *re, float *im) {
  const int N = KWS_FRAME_LEN;
  for (int i = 0; i < N; i++) {
    int j = s_bitrev[i];
    if (j > i) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= N; len <<= 1) {
    int half = len >> 1, step = N / len;
    for (int s = 0; s < N; s += len) {
      for (int k = 0; k < half; k++) {
        float wr = s_cos[k * step], wi = s_sin[k * step];
        int a = s + k, b = a + half;
        float tr = re[b] * wr - im[b] * wi;
        float ti = re[b] * wi + im[b] * wr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr;        im[a] += ti;
      }
    }
  }
}

void kws_features_frame(const int16_t *frame, float *feat) {
  const int N = KWS_FRAME_LEN;

  // 1) RMS no domínio do tempo (amostras normalizadas para [-1, 1)).
  float sumsq = 0.0f;
  for (int n = 0; n < N; n++) {
    float x = frame[n] * (1.0f / 32768.0f);
    sumsq += x * x;
    s_re[n] = x * s_window[n];  // 2) janela de Hann (reduz vazamento espectral)
    s_im[n] = 0.0f;
  }
  float rms = sqrtf(sumsq / N);

  // 3) FFT -> espectro de potência |X[k]|^2, k = 0..256 (0 Hz .. 8 kHz, passo 31,25 Hz)
  fft(s_re, s_im);
  float psum = 0.0f, fsum = 0.0f;
  const float bin_hz = (float)KWS_SAMPLE_RATE / N;
  for (int k = 0; k < KWS_N_BINS; k++) {
    float p = s_re[k] * s_re[k] + s_im[k] * s_im[k];
    s_pow[k] = p;
    psum += p;
    fsum += p * (k * bin_hz);
  }

  // 4) Spectral centroid = média das frequências ponderada pela potência.
  float centroid = (psum > 0.0f) ? fsum / psum : 0.0f;

  // 5) Energia em cada filtro mel -> log -> DCT = MFCCs.
  float logmel[KWS_N_MEL];
  for (int m = 0; m < KWS_N_MEL; m++) {
    float e = 0.0f;
    const float *w = &s_mel_w[s_mel_off[m]];
    for (int j = 0; j < s_mel_len[m]; j++) e += w[j] * s_pow[s_mel_start[m] + j];
    logmel[m] = logf(e + KWS_LOG_EPS);
  }
  for (int i = 0; i < KWS_N_MFCC; i++) {
    float c = 0.0f;
    for (int m = 0; m < KWS_N_MEL; m++) c += s_dct[i][m] * logmel[m];
    feat[i] = c;
  }
  feat[13] = log10f(rms + KWS_LOG_EPS);
  feat[14] = centroid / (KWS_SAMPLE_RATE / 2.0f);
}

// ------------------------------------------------------------------ VAD
void kws_vad_reset(kws_vad_t *v) {
  v->floor_db = 0.0f;
  v->above = 0;
  v->init = 0;
}

int kws_vad_update(kws_vad_t *v, float log_rms) {
  float db = 20.0f * log_rms;  // log10(RMS) -> dBFS
  if (!v->init) {
    v->floor_db = db;
    v->init = 1;
  }
  int is_speech = (db > v->floor_db + KWS_VAD_ON_DB) && (db > KWS_VAD_MIN_DB);
  int onset = 0;
  if (is_speech) {
    v->above++;
    if (v->above == KWS_VAD_ON_FRAMES) onset = 1;  // dispara uma única vez por subida
    // "Fala" longa demais = provavelmente ruído que ficou permanente: deixa o piso subir.
    if (v->above > KWS_VAD_MAX_FRAMES) v->floor_db += KWS_VAD_FLOOR_UP * (db - v->floor_db);
  } else {
    v->above = 0;
    // Piso de ruído é atualizado fora da fala: desce rápido, sobe devagar.
    float rate = (db < v->floor_db) ? KWS_VAD_FLOOR_DOWN : KWS_VAD_FLOOR_UP;
    v->floor_db += rate * (db - v->floor_db);
  }
  return onset;
}
