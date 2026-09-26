"""Leitura do dataset e construção das janelas de 61 frames (com data augmentation).

A janela é escolhida com a MESMA regra do ESP32: detector de fala (VAD) por
energia -> início da fala -> janela começa PRE_ROLL frames antes.
"""
import csv
import os
import wave

import numpy as np

import features as F
from model import CLASSES


def read_wav(path):
    with wave.open(path) as w:
        assert w.getframerate() == F.SR and w.getnchannels() == 1 and w.getsampwidth() == 2, path
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").copy()


def load_manifest(root):
    """Lista de clipes existentes no disco (linhas de arquivos apagados são ignoradas)."""
    rows = {}
    with open(os.path.join(root, "manifest.csv"), encoding="utf-8") as f:
        for r in csv.DictReader(f):
            rows[r["file"]] = r
    clips = []
    for rel, r in rows.items():
        path = os.path.join(root, rel)
        if os.path.exists(path) and r["label"] in CLASSES:
            clips.append({**r, "path": path})
    clips.sort(key=lambda c: c["file"])
    return clips


def _speech_power(x):
    fr = F.clip_frames(x).astype(np.float64)
    e = np.mean(fr**2, axis=1)
    return float(np.mean(np.sort(e)[-max(1, len(e) // 4):])) + 1e-9


def eval_start(feats, label):
    """Início da janela determinística (a que o ESP32 veria). Retorna (inicio, disparou_vad)."""
    onset = F.find_onset(feats)
    if onset is not None:
        return F.window_start(feats, onset), True
    if label == "noise":
        return max(0, (len(feats) - F.WIN_FRAMES) // 2), False
    # fala não detectada pelo VAD: centraliza no frame mais energético
    return int(np.clip(np.argmax(feats[:, 13]) - F.WIN_FRAMES // 3, 0, len(feats) - F.WIN_FRAMES)), False


def eval_window(feats, label):
    s, fired = eval_start(feats, label)
    return feats[s:s + F.WIN_FRAMES], fired


def augmented_windows(x, label, rng, n_aug, bg_pool):
    """Versões aumentadas de um clipe: ganho, ruído de ambiente, deslocamento."""
    out = []
    for _ in range(n_aug):
        y = x.astype(np.float64) * 10 ** (rng.uniform(-6, 6) / 20)
        if bg_pool and rng.random() < 0.6:
            bg = bg_pool[rng.integers(len(bg_pool))]
            if len(bg) >= len(y):
                off = rng.integers(0, len(bg) - len(y) + 1)
                n = bg[off:off + len(y)].astype(np.float64)
                snr = rng.uniform(5, 25)
                pn = np.mean(n**2) + 1e-9
                y = y + n * np.sqrt(_speech_power(x) / pn / 10 ** (snr / 10))
        y = np.clip(np.round(y), -32768, 32767).astype(np.int16)
        feats = F.clip_features(y)
        if label == "noise":
            s = int(rng.integers(0, len(feats) - F.WIN_FRAMES + 1))
        else:
            s0, _ = eval_start(feats, label)
            s = int(np.clip(s0 + rng.integers(-4, 5), 0, len(feats) - F.WIN_FRAMES))
        out.append(feats[s:s + F.WIN_FRAMES])
    return out
