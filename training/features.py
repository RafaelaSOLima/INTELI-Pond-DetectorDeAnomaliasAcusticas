"""Extração de features — espelho EXATO de firmware/libraries/kws_common/src/kws_features.c.

As constantes são lidas do próprio kws_features.h, então C e Python nunca
divergem nos parâmetros. tests/test_features.py compila o C e compara os dois.
"""
import os
import re

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(_HERE, "..", "firmware", "libraries", "kws_common", "src", "kws_features.h")


def _read_defines(path):
    out = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"#define\s+(KWS_\w+)\s+([^/]+)", line)
        if m:
            name, val = m.group(1), m.group(2).strip()
            try:
                out[name] = float(val.rstrip("f")) if re.fullmatch(r"-?[\d.e+-]+f?", val) else val
            except ValueError:
                out[name] = val
    return out


_D = _read_defines(HEADER)
SR = int(_D["KWS_SAMPLE_RATE"])
FRAME = int(_D["KWS_FRAME_LEN"])
HOP = int(_D["KWS_HOP"])
N_BINS = FRAME // 2 + 1
N_MEL = int(_D["KWS_N_MEL"])
N_MFCC = int(_D["KWS_N_MFCC"])
N_FEAT = int(_D["KWS_N_FEAT"])
FMIN, FMAX = _D["KWS_MEL_FMIN"], _D["KWS_MEL_FMAX"]
EPS = np.float32(_D["KWS_LOG_EPS"])
WIN_FRAMES = int(_D["KWS_WIN_FRAMES"])
VAD_ON_DB = _D["KWS_VAD_ON_DB"]
VAD_MIN_DB = _D["KWS_VAD_MIN_DB"]
VAD_ON_FRAMES = int(_D["KWS_VAD_ON_FRAMES"])
VAD_FLOOR_UP = _D["KWS_VAD_FLOOR_UP"]
VAD_FLOOR_DOWN = _D["KWS_VAD_FLOOR_DOWN"]
VAD_MAX_FRAMES = int(_D["KWS_VAD_MAX_FRAMES"])
PRE_ROLL = int(_D["KWS_PRE_ROLL_FRAMES"])

FEATURE_NAMES = [f"mfcc{i}" for i in range(N_MFCC)] + ["log_rms", "centroid"]


def _tables():
    n = np.arange(FRAME)
    window = (0.5 - 0.5 * np.cos(2 * np.pi * n / FRAME)).astype(np.float32)
    hz2mel = lambda hz: 2595.0 * np.log10(1.0 + hz / 700.0)
    mel2hz = lambda mel: 700.0 * (10 ** (mel / 2595.0) - 1.0)
    lo, hi = hz2mel(FMIN), hz2mel(FMAX)
    pts = mel2hz(lo + (hi - lo) * np.arange(N_MEL + 2) / (N_MEL + 1))
    f = np.arange(N_BINS) * SR / FRAME
    fb = np.zeros((N_MEL, N_BINS), np.float32)
    for m in range(N_MEL):
        a, c, b = pts[m], pts[m + 1], pts[m + 2]
        up = (f > a) & (f < c)
        down = (f >= c) & (f < b)
        fb[m, up] = (f[up] - a) / (c - a)
        fb[m, down] = (b - f[down]) / (b - c)
    i = np.arange(N_MFCC)[:, None]
    m = np.arange(N_MEL)[None, :]
    dct = np.cos(np.pi * i * (m + 0.5) / N_MEL) * np.sqrt(2.0 / N_MEL)
    dct[0] *= np.sqrt(0.5)  # sqrt(1/M) para o coeficiente 0
    return window, fb, dct.astype(np.float32)


WINDOW, MELFB, DCT = _tables()


def frame_features(frames_i16):
    """frames_i16: (n, FRAME) int16 -> (n, N_FEAT) float32."""
    x = frames_i16.astype(np.float32) / np.float32(32768.0)
    rms = np.sqrt(np.mean(x * x, axis=1))
    spec = np.fft.rfft(x * WINDOW, axis=1)
    p = (spec.real**2 + spec.imag**2).astype(np.float32)
    freqs = np.arange(N_BINS, dtype=np.float32) * np.float32(SR / FRAME)
    psum = p.sum(axis=1)
    centroid = np.where(psum > 0, (p * freqs).sum(axis=1) / np.maximum(psum, 1e-30), 0.0)
    logmel = np.log(p @ MELFB.T + EPS)
    mfcc = logmel @ DCT.T
    out = np.concatenate(
        [mfcc, np.log10(rms + EPS)[:, None], (centroid / (SR / 2.0))[:, None]], axis=1
    )
    return out.astype(np.float32)


def clip_frames(x):
    """Divide o sinal int16 em frames de FRAME amostras com passo HOP."""
    n = 1 + (len(x) - FRAME) // HOP if len(x) >= FRAME else 0
    idx = np.arange(FRAME)[None, :] + HOP * np.arange(n)[:, None]
    return x[idx]


def clip_features(x):
    return frame_features(clip_frames(np.asarray(x, dtype=np.int16)))


class Vad:
    """Espelho de kws_vad_update()."""

    def __init__(self):
        self.floor_db, self.above, self.init = 0.0, 0, False

    def update(self, log_rms):
        db = 20.0 * float(log_rms)
        if not self.init:
            self.floor_db, self.init = db, True
        speech = db > self.floor_db + VAD_ON_DB and db > VAD_MIN_DB
        onset = False
        if speech:
            self.above += 1
            onset = self.above == VAD_ON_FRAMES
            if self.above > VAD_MAX_FRAMES:
                self.floor_db += VAD_FLOOR_UP * (db - self.floor_db)
        else:
            self.above = 0
            rate = VAD_FLOOR_DOWN if db < self.floor_db else VAD_FLOOR_UP
            self.floor_db += rate * (db - self.floor_db)
        return onset


def find_onset(feats):
    """Frame do primeiro início de fala (mesma regra do ESP32) ou None."""
    v = Vad()
    for i, lr in enumerate(feats[:, 13]):
        if v.update(lr):
            return i
    return None


def window_start(feats, onset):
    """Início da janela de classificação = onset - PRE_ROLL, limitado ao clipe."""
    return int(np.clip(onset - PRE_ROLL, 0, max(len(feats) - WIN_FRAMES, 0)))
