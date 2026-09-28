#!/usr/bin/env python3
"""Converte model/kws.onnx em um header C com os pesos, para a inferência no ESP32.

    python3 training/export_c.py --onnx model/kws.onnx --meta model/metadata.json

1) Confere que o grafo ONNX é EXATAMENTE a sequência de operações que o C
   implementa (kws_model.c). Se o modelo mudar, este script recusa.
2) Lê os pesos (initializers) do próprio .onnx — o .onnx é a fonte da verdade.
3) Escreve firmware/libraries/kws_common/src/kws_model_weights.h.
"""
import argparse
import datetime as dt
import hashlib
import json
import os

import numpy as np
import onnx
from onnx import numpy_helper

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "firmware", "libraries", "kws_common", "src", "kws_model_weights.h")

EXPECTED = ["Sub", "Div", "Conv", "Relu", "MaxPool", "Conv", "Relu", "MaxPool", "Flatten",
            "Gemm", "Relu", "Gemm", "Softmax"]


def attr(node):
    return {a.name: onnx.helper.get_attribute_value(a) for a in node.attribute}


def c_array(name, arr):
    flat = np.asarray(arr, dtype=np.float32).ravel()
    body = ",\n".join("  " + ", ".join(f"{v:.9g}f" for v in flat[i:i + 8]) for i in range(0, len(flat), 8))
    return f"static const float {name}[{len(flat)}] = {{\n{body}\n}};\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", default=os.path.join(ROOT, "model", "kws.onnx"))
    ap.add_argument("--meta", default=os.path.join(ROOT, "model", "metadata.json"))
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--placeholder", action="store_true", help="marca o header como modelo NÃO real")
    args = ap.parse_args()

    m = onnx.load(args.onnx)
    onnx.checker.check_model(m)
    ops = [n.op_type for n in m.graph.node]
    if ops != EXPECTED:
        raise SystemExit(f"grafo inesperado:\n  {ops}\nesperado:\n  {EXPECTED}\natualize kws_model.c e este script")
    init = {i.name: numpy_helper.to_array(i) for i in m.graph.initializer}
    nodes = m.graph.node
    for i in (2, 5):
        a = attr(nodes[i])
        assert a["kernel_shape"] == [3, 3] and a["pads"] == [1, 1, 1, 1] and a["strides"] == [1, 1] and a.get("group", 1) == 1
    for i in (4, 7):
        a = attr(nodes[i])
        assert a["kernel_shape"] == [2, 2] and a["strides"] == [2, 2] and a.get("ceil_mode", 0) == 0
    for i in (9, 11):
        a = attr(nodes[i])
        assert a.get("transB", 0) == 1 and a.get("alpha", 1.0) == 1.0 and a.get("beta", 1.0) == 1.0
    assert attr(nodes[8]).get("axis", 1) == 1 and attr(nodes[12]).get("axis", 1) in (1, -1)

    mean = init[nodes[0].input[1]].ravel()
    std = init[nodes[1].input[1]].ravel()
    c1w, c1b = init[nodes[2].input[1]], init[nodes[2].input[2]]
    c2w, c2b = init[nodes[5].input[1]], init[nodes[5].input[2]]
    f1w, f1b = init[nodes[9].input[1]], init[nodes[9].input[2]]
    f2w, f2b = init[nodes[11].input[1]], init[nodes[11].input[2]]

    dims = m.graph.input[0].type.tensor_type.shape.dim
    frames, nfeat = dims[2].dim_value, dims[3].dim_value
    assert c1w.shape[1] == 1 and c2w.shape[1] == c1w.shape[0]
    h2, w2 = frames // 2 // 2, nfeat // 2 // 2
    assert f1w.shape[1] == c2w.shape[0] * h2 * w2, "fc1 incompatível com a saída das convoluções"

    meta = json.load(open(args.meta)) if os.path.exists(args.meta) else {}
    classes = meta.get("classes", ["ball", "cat", "dog", "unknown", "noise"])
    thr = float(meta.get("threshold", 0.7))
    digest = hashlib.sha256(open(args.onnx, "rb").read()).hexdigest()[:16]

    h = [
        "// ARQUIVO GERADO por training/export_c.py — NÃO EDITE À MÃO.",
        f"// Origem: {os.path.relpath(args.onnx, ROOT)} (sha256 {digest}) em {dt.datetime.now().isoformat(timespec='seconds')}",
        "#pragma once",
        "",
        f"#define KWS_MODEL_IS_PLACEHOLDER {1 if args.placeholder else 0}"
        + ("  // ATENÇÃO: pesos de um modelo SINTÉTICO de teste, não reconhece voz real" if args.placeholder else ""),
        f'#define KWS_MODEL_SHA "{digest}"',
        f"#define KWS_MODEL_FRAMES {frames}",
        f"#define KWS_MODEL_FEATS {nfeat}",
        f"#define KWS_MODEL_C1 {c1w.shape[0]}",
        f"#define KWS_MODEL_C2 {c2w.shape[0]}",
        f"#define KWS_MODEL_FC1 {f1w.shape[0]}",
        f"#define KWS_MODEL_N_CLASSES {f2w.shape[0]}",
        f"#define KWS_MODEL_THRESHOLD {thr:.4f}f",
        "static const char *const KWS_MODEL_CLASSES[KWS_MODEL_N_CLASSES] = {"
        + ", ".join(f'"{c}"' for c in classes) + "};",
        "",
        c_array("kws_w_mean", mean), c_array("kws_w_std", std),
        c_array("kws_w_conv1", c1w), c_array("kws_b_conv1", c1b),
        c_array("kws_w_conv2", c2w), c_array("kws_b_conv2", c2b),
        c_array("kws_w_fc1", f1w), c_array("kws_b_fc1", f1b),
        c_array("kws_w_fc2", f2w), c_array("kws_b_fc2", f2b),
    ]
    with open(args.out, "w") as f:
        f.write("\n".join(h))
    n = sum(a.size for a in (mean, std, c1w, c1b, c2w, c2b, f1w, f1b, f2w, f2b))
    print(f"escrito {os.path.relpath(args.out, ROOT)}: {n} valores float ({n * 4 / 1024:.1f} KB), threshold {thr}")


if __name__ == "__main__":
    main()
