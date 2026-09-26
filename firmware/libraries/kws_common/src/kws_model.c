// kws_model.c — ver kws_model.h. Layout dos tensores: NCHW (canal, linha, coluna),
// igual ao ONNX. "linha" = frame no tempo, "coluna" = índice da feature.
#include "kws_model.h"

#include <math.h>

#define H0 KWS_MODEL_FRAMES          // 61
#define W0 KWS_MODEL_FEATS           // 15
#define H1 (H0 / 2)                  // 30 após o 1º MaxPool
#define W1 (W0 / 2)                  // 7
#define H2 (H1 / 2)                  // 15 após o 2º MaxPool
#define W2 (W1 / 2)                  // 3
#define FLAT (KWS_MODEL_C2 * H2 * W2)

// Buffers intermediários (estáticos: ficam na RAM global, não na stack da task).
static float s_x0[H0 * W0];
static float s_c1[KWS_MODEL_C1 * H0 * W0];
static float s_p1[KWS_MODEL_C1 * H1 * W1];
static float s_c2[KWS_MODEL_C2 * H1 * W1];
static float s_p2[FLAT];
static float s_f1[KWS_MODEL_FC1];

// Convolução 3x3, stride 1, padding 1 ("same"), seguida de ReLU.
static void conv3x3_relu(const float *in, int C, int H, int W, const float *w, const float *b, int OC, float *out) {
  for (int oc = 0; oc < OC; oc++) {
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        float acc = b[oc];
        for (int c = 0; c < C; c++) {
          const float *wk = &w[((oc * C + c) * 3) * 3];
          const float *ic = &in[c * H * W];
          for (int ky = 0; ky < 3; ky++) {
            int iy = y + ky - 1;
            if (iy < 0 || iy >= H) continue;  // padding com zeros
            for (int kx = 0; kx < 3; kx++) {
              int ix = x + kx - 1;
              if (ix < 0 || ix >= W) continue;
              acc += wk[ky * 3 + kx] * ic[iy * W + ix];
            }
          }
        }
        out[(oc * H + y) * W + x] = acc > 0.0f ? acc : 0.0f;  // ReLU
      }
    }
  }
}

// MaxPool 2x2, stride 2, sem padding (sobras ímpares são descartadas, como no ONNX).
static void maxpool2(const float *in, int C, int H, int W, float *out) {
  int OH = H / 2, OW = W / 2;
  for (int c = 0; c < C; c++)
    for (int y = 0; y < OH; y++)
      for (int x = 0; x < OW; x++) {
        const float *p = &in[(c * H + 2 * y) * W + 2 * x];
        float m = p[0];
        if (p[1] > m) m = p[1];
        if (p[W] > m) m = p[W];
        if (p[W + 1] > m) m = p[W + 1];
        out[(c * OH + y) * OW + x] = m;
      }
}

// Camada densa (Gemm com transB=1): out[o] = b[o] + sum_i w[o][i] * in[i]
static void dense(const float *in, int N, const float *w, const float *b, int O, float *out, int relu) {
  for (int o = 0; o < O; o++) {
    float acc = b[o];
    const float *wo = &w[o * N];
    for (int i = 0; i < N; i++) acc += wo[i] * in[i];
    out[o] = (relu && acc < 0.0f) ? 0.0f : acc;
  }
}

void kws_model_run(const float *in, float *probs) {
  // Sub + Div: normalização por feature (média/desvio do treino)
  for (int t = 0; t < H0; t++)
    for (int f = 0; f < W0; f++) s_x0[t * W0 + f] = (in[t * W0 + f] - kws_w_mean[f]) / kws_w_std[f];

  conv3x3_relu(s_x0, 1, H0, W0, kws_w_conv1, kws_b_conv1, KWS_MODEL_C1, s_c1);
  maxpool2(s_c1, KWS_MODEL_C1, H0, W0, s_p1);
  conv3x3_relu(s_p1, KWS_MODEL_C1, H1, W1, kws_w_conv2, kws_b_conv2, KWS_MODEL_C2, s_c2);
  maxpool2(s_c2, KWS_MODEL_C2, H1, W1, s_p2);  // s_p2 já está "achatado" em ordem C,H,W (Flatten)
  dense(s_p2, FLAT, kws_w_fc1, kws_b_fc1, KWS_MODEL_FC1, s_f1, 1);
  float logits[KWS_MODEL_N_CLASSES];
  dense(s_f1, KWS_MODEL_FC1, kws_w_fc2, kws_b_fc2, KWS_MODEL_N_CLASSES, logits, 0);

  // Softmax numericamente estável: subtrai o máximo antes do exp.
  float mx = logits[0], sum = 0.0f;
  for (int k = 1; k < KWS_MODEL_N_CLASSES; k++) if (logits[k] > mx) mx = logits[k];
  for (int k = 0; k < KWS_MODEL_N_CLASSES; k++) { probs[k] = expf(logits[k] - mx); sum += probs[k]; }
  for (int k = 0; k < KWS_MODEL_N_CLASSES; k++) probs[k] /= sum;
}
