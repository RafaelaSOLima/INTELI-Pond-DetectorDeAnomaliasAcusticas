#!/usr/bin/env python3
"""Prova que a inferência em C (ESP32) dá o mesmo resultado que o onnxruntime.

Compila kws_model.c (com o kws_model_weights.h atual) via gcc, roda em entradas
aleatórias e em janelas reais do dataset, e compara com o .onnx.

    python3 tests/test_model_c.py [--onnx model/kws.onnx] [--root dataset/raw]
"""
import argparse
import ctypes
import glob
import os
import subprocess
import sys
import tempfile

import numpy as np
import onnxruntime as ort

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "training"))
import features as F  # noqa: E402
from dataset import eval_window, read_wav  # noqa: E402

SRC = os.path.join(ROOT, "firmware", "libraries", "kws_common", "src", "kws_model.c")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", default=os.path.join(ROOT, "model", "kws.onnx"))
    ap.add_argument("--root", default=os.path.join(ROOT, "dataset", "raw"))
    args = ap.parse_args()

    so = os.path.join(tempfile.mkdtemp(), "libkwsmodel.so")
    subprocess.run(["gcc", "-O2", "-shared", "-fPIC", "-Wall", "-Wextra", "-Wno-unused-variable",
                    "-o", so, SRC, "-lm"], check=True)
    lib = ctypes.CDLL(so)
    lib.kws_model_run.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float)]
    sess = ort.InferenceSession(args.onnx)
    n_classes = sess.get_outputs()[0].shape[1]

    rng = np.random.default_rng(0)
    inputs = [("aleatória", rng.normal(0, 3, (F.WIN_FRAMES, F.N_FEAT)).astype(np.float32)) for _ in range(20)]
    for p in sorted(glob.glob(os.path.join(args.root, "*", "*.wav")))[::7][:60]:
        label = os.path.basename(os.path.dirname(p))
        w, _ = eval_window(F.clip_features(read_wav(p)), label)
        inputs.append((os.path.relpath(p, args.root), np.ascontiguousarray(w, dtype=np.float32)))

    worst, agree = 0.0, 0
    for name, x in inputs:
        pc = np.zeros(n_classes, np.float32)
        lib.kws_model_run(x.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), pc.ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
        po = sess.run(None, {"features": x[None, None]})[0][0]
        err = float(np.abs(pc - po).max())
        worst = max(worst, err)
        agree += int(pc.argmax() == po.argmax())
    print(f"{len(inputs)} entradas | maior diferença de probabilidade C vs ONNX: {worst:.2e} | "
          f"mesma classe prevista: {agree}/{len(inputs)}")
    ok = worst < 1e-4 and agree == len(inputs)
    print("OK" if ok else "FALHOU")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
