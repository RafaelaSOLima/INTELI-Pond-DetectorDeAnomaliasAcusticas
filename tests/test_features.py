#!/usr/bin/env python3
"""Prova que as features em C (ESP32) e em Python (treino) são iguais.

Compila firmware/libraries/kws_common/src/kws_features.c com gcc como
biblioteca compartilhada, chama pelo ctypes e compara com training/features.py
em vários sinais (e em WAVs reais do dataset, se existirem).

    python3 tests/test_features.py
"""
import ctypes
import glob
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "training"))
import features as F  # noqa: E402

SRC = os.path.join(ROOT, "firmware", "libraries", "kws_common", "src", "kws_features.c")


class VadT(ctypes.Structure):
    _fields_ = [("floor_db", ctypes.c_float), ("above", ctypes.c_int), ("init", ctypes.c_int)]


def build_lib():
    out = os.path.join(tempfile.mkdtemp(), "libkwsfeat.so")
    subprocess.run(["gcc", "-O2", "-shared", "-fPIC", "-Wall", "-Wextra", "-o", out, SRC, "-lm"], check=True)
    lib = ctypes.CDLL(out)
    lib.kws_features_frame.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.POINTER(ctypes.c_float)]
    lib.kws_vad_update.argtypes = [ctypes.POINTER(VadT), ctypes.c_float]
    lib.kws_vad_update.restype = ctypes.c_int
    lib.kws_features_init()
    return lib


def c_clip_features(lib, x):
    frames = F.clip_frames(x.astype(np.int16))
    out = np.zeros((len(frames), F.N_FEAT), np.float32)
    for i, fr in enumerate(frames):
        fr = np.ascontiguousarray(fr, dtype=np.int16)
        lib.kws_features_frame(fr.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)),
                               out[i].ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
    return out


def c_onset(lib, feats):
    v = VadT()
    lib.kws_vad_reset(ctypes.byref(v))
    for i, lr in enumerate(feats[:, 13]):
        if lib.kws_vad_update(ctypes.byref(v), ctypes.c_float(lr)):
            return i
    return None


def signals():
    rng = np.random.default_rng(0)
    sr, n = F.SR, 24000
    t = np.arange(n) / sr
    yield "silencio (zeros)", np.zeros(n)
    yield "ruido fraco", rng.normal(0, 30, n)
    yield "ruido forte", rng.normal(0, 8000, n)
    yield "senoide 440 Hz", 8000 * np.sin(2 * np.pi * 440 * t)
    yield "varredura 100-7000 Hz", 6000 * np.sin(2 * np.pi * (100 * t + 3450 * t**2 / 1.5))
    word = np.zeros(n)
    s, L = 7000, 6400
    tt = np.arange(L) / sr
    word[s:s + L] = sum(np.sin(2 * np.pi * 180 * k * tt) / k for k in range(1, 8)) * np.hanning(L) * 9000
    yield "palavra sintetica + ruido", word + rng.normal(0, 60, n)
    yield "palavra baixa", word * 0.02 + rng.normal(0, 5, n)
    yield "saturado", np.clip(word * 8, -32768, 32767)


def read_wav(path):
    with wave.open(path) as w:
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


def main():
    lib = build_lib()
    cases = list(signals())
    wavs = sorted(glob.glob(os.path.join(ROOT, "dataset", "raw", "*", "*.wav")))
    step = max(1, len(wavs) // 40)
    cases += [(os.path.relpath(p, ROOT), read_wav(p)) for p in wavs[::step]]

    worst = np.zeros(F.N_FEAT)
    fails = 0
    for name, x in cases:
        x = np.clip(np.round(x), -32768, 32767).astype(np.int16)
        fc, fp = c_clip_features(lib, x), F.clip_features(x)
        err = np.abs(fc - fp).max(axis=0)
        worst = np.maximum(worst, err)
        # Tolerâncias: MFCC em unidades de log natural (1e-2 é inaudível/irrelevante),
        # log_rms em log10, centroid normalizado 0..1.
        tol = np.array([2e-2] * F.N_MFCC + [1e-4, 1e-4])
        ok_feat = bool(np.all(err <= tol))
        oc, op = c_onset(lib, fc), F.find_onset(fp)
        ok_vad = oc == op
        status = "OK " if ok_feat and ok_vad else "FALHOU"
        fails += not (ok_feat and ok_vad)
        print(f"{status} {name:45s} frames={len(fc):3d}  erro max mfcc={err[:13].max():.2e} "
              f"rms={err[13]:.2e} centroid={err[14]:.2e}  onset C={oc} Py={op}")
    print("\nMaior erro por feature:", " ".join(f"{n}={e:.1e}" for n, e in zip(F.FEATURE_NAMES, worst)))
    print(f"\n{len(cases) - fails}/{len(cases)} casos OK")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
