"""Leitura do dataset e construção das janelas de 61 frames (com data augmentation).

A janela é escolhida com a MESMA regra do ESP32: detector de fala (VAD) por
energia -> início da fala -> janela começa PRE_ROLL frames antes.
"""
import csv
import os
import wave

import numpy as np

import features as F
from labels import CLASSES


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


def pad_clip(x):
    """Estende o clipe com o PRÓPRIO ruído de fundo: 1,0 s antes e 0,8 s depois.

    No ESP32 o áudio é contínuo: o VAD já conhece o piso de ruído quando a fala
    começa, e sempre há áudio suficiente depois do início para completar a
    janela de 61 frames. Nos clipes de 1,5 s isso não vale quando a pessoa fala
    tarde. Com o preenchimento, a janela de treino = a janela que o ESP32 vê.
    """
    # Trecho de 200 ms MAIS SILENCIOSO do clipe (não o início: muita gente fala
    # logo no começo e repetir o início repetiria pedaços da palavra).
    seg = 3200
    starts = np.arange(0, max(len(x) - seg, 0) + 1, 400)
    energy = [float(np.mean(x[s:s + seg].astype(np.float64) ** 2)) for s in starts]
    bg = x[starts[int(np.argmin(energy))]:][:seg]
    pre = np.tile(bg, 5)[:16000]
    post = np.tile(bg, 4)[:12800]
    s = np.concatenate([pre, x, post]).astype(np.int16)
    pad = (-len(s)) % F.HOP
    return np.concatenate([s, np.zeros(pad, np.int16)])


def _speech_power(x):
    fr = F.clip_frames(x).astype(np.float64)
    e = np.mean(fr**2, axis=1)
    return float(np.mean(np.sort(e)[-max(1, len(e) // 4):])) + 1e-9


# Posição do núcleo (frame mais energético) da palavra dentro da janela — a
# mesma constante que o ESP32 usa (KWS_PEAK_POS em kws_features.h).
PEAK_POS = F.PEAK_POS


def eval_start(feats, label):
    """Início da janela de 61 frames usada no treino/avaliação.

    Nas gravações do dataset o VAD às vezes disparava no clique da tecla Enter
    (primeiros ~60 ms do clipe), antes da palavra. Por isso posicionamos a janela
    pelo núcleo de energia da palavra, no mesmo lugar em que ele cai no ESP32.
    Retorna (inicio, vad_disparou) — o segundo valor é só informativo.
    """
    onset = F.find_onset(feats)
    last = len(feats) - F.WIN_FRAMES
    if label == "noise":
        return max(0, last // 2), onset is not None
    peak = int(np.argmax(feats[:, 13]))
    return int(np.clip(peak - PEAK_POS, 0, last)), onset is not None


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
            if len(bg) < len(y):  # ambiente mais curto que o clipe preenchido: repete
                bg = np.tile(bg, int(np.ceil(len(y) / len(bg))))
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
            s = int(np.clip(s0 + rng.integers(-6, 7), 0, len(feats) - F.WIN_FRAMES))
        out.append(feats[s:s + F.WIN_FRAMES])
    return out
