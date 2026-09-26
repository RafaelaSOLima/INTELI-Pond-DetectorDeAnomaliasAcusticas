// kws_model.h — inferência da CNN em C puro (mesmas operações do kws.onnx).
//
// Grafo ONNX implementado (verificado por training/export_c.py):
//   Sub -> Div -> Conv3x3 -> Relu -> MaxPool2 -> Conv3x3 -> Relu -> MaxPool2
//   -> Flatten -> Gemm -> Relu -> Gemm -> Softmax
// Os pesos vêm de kws_model_weights.h, gerado a partir do .onnx.
#pragma once
#include "kws_model_weights.h"

#ifdef __cplusplus
extern "C" {
#endif

// in:    KWS_MODEL_FRAMES x KWS_MODEL_FEATS features (linha = frame), sem normalizar
// probs: KWS_MODEL_N_CLASSES probabilidades (somam 1)
// Usa buffers internos estáticos: só UMA task pode chamar (no ESP32, a Task 3).
void kws_model_run(const float *in, float *probs);

#ifdef __cplusplus
}
#endif
