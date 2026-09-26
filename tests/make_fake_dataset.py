#!/usr/bin/env python3
"""Gera um dataset SINTÉTICO no mesmo formato do real, só para testar o pipeline
(treino, export ONNX, C) sem depender das gravações. Não serve para avaliar o modelo.

    python3 tests/make_fake_dataset.py --out /tmp/fake_raw --speakers 4
"""
import argparse
import csv
import os
import sys
import wave

import numpy as np

SR = 16000
N = 24000  # 1,5 s


def vowel(f0, f1, f2, dur, rng):
    t = np.arange(int(dur * SR)) / SR
    f0t = f0 * (1 + 0.05 * np.sin(2 * np.pi * 3 * t))
    ph = 2 * np.pi * np.cumsum(f0t) / SR
    y = np.zeros_like(t)
    for k in range(1, 40):
        fk = k * f0
        if fk > 7500:
            break
        amp = np.exp(-((fk - f1) / 250) ** 2) + 0.6 * np.exp(-((fk - f2) / 350) ** 2) + 0.02
        y += amp * np.sin(k * ph + rng.uniform(0, 6.28))
    return y * np.hanning(len(t)) ** 0.3


def burst(center, bw, dur, rng):
    n = int(dur * SR)
    x = rng.normal(0, 1, n)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1 / SR)
    X *= np.exp(-((f - center) / bw) ** 2)
    return np.fft.irfft(X, n) * np.hanning(n) * 3


WORDS = {  # sequência de segmentos: ("v", F1, F2, dur) ou ("b", centro, banda, dur)
    "ball": [("b", 300, 200, 0.03), ("v", 600, 900, 0.25), ("v", 400, 800, 0.12)],
    "cat": [("b", 3000, 800, 0.06), ("v", 750, 1750, 0.18), ("b", 4000, 1000, 0.05)],
    "dog": [("b", 500, 300, 0.04), ("v", 550, 1000, 0.22), ("b", 1500, 500, 0.05)],
}


def make_word(segs, f0, rng):
    parts = []
    for s in segs:
        if s[0] == "v":
            parts.append(vowel(f0, s[1] * rng.uniform(0.9, 1.1), s[2] * rng.uniform(0.9, 1.1), s[3] * rng.uniform(0.8, 1.25), rng))
        else:
            parts.append(burst(s[1], s[2], s[3], rng))
        parts.append(np.zeros(int(0.01 * SR)))
    return np.concatenate(parts)


def place(sig, rng, level, noise=40):
    x = rng.normal(0, noise, N)
    sig = sig / (np.abs(sig).max() + 1e-9) * level
    s = rng.integers(int(0.25 * SR), N - len(sig) - int(0.1 * SR))
    x[s:s + len(sig)] += sig
    return np.clip(x, -32768, 32767).astype("<i2")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/fake_raw")
    ap.add_argument("--speakers", type=int, default=4)
    ap.add_argument("--reps", type=int, default=12)
    args = ap.parse_args()
    rng = np.random.default_rng(1)
    rows = []

    def save(label, spk, word, i, x):
        d = os.path.join(args.out, label)
        os.makedirs(d, exist_ok=True)
        fn = f"{spk}_{word}_fake_{i:03d}.wav"
        with wave.open(os.path.join(d, fn), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes(x.tobytes())
        rows.append([f"{label}/{fn}", spk, label, word, "fake", "sim", "medio", "", "", "", ""])

    for s in range(args.speakers):
        spk = f"s{s + 1:02d}"
        f0 = rng.uniform(110, 280)
        for w, segs in WORDS.items():
            for i in range(args.reps):
                save(w, spk, w, i, place(make_word(segs, f0 * rng.uniform(0.9, 1.1), rng), rng, rng.uniform(2000, 15000)))
        for i in range(args.reps):
            segs = [("v", rng.uniform(300, 900), rng.uniform(900, 2500), rng.uniform(0.15, 0.4)) for _ in range(rng.integers(1, 3))]
            if rng.random() < 0.5:
                segs.insert(0, ("b", rng.uniform(300, 5000), 500, 0.04))
            save("unknown", spk, "outra", i, place(make_word(segs, f0, rng), rng, rng.uniform(2000, 15000)))
        for i in range(args.reps // 2):
            x = burst(rng.uniform(500, 5000), 3000, 0.05, rng) if i % 2 else np.zeros(100)
            save("noise", spk, "palma" if i % 2 else "silencio", i, place(x + 1e-3, rng, rng.uniform(100, 20000) if i % 2 else 1))
    for i in range(12):
        x = (rng.normal(0, rng.uniform(20, 200), N)).astype("<i2")
        save("noise", "bg", "ambiente", i, x)
    with open(os.path.join(args.out, "manifest.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["file", "speaker", "label", "word", "device", "env", "distance", "peak_dbfs", "rms_dbfs", "warnings", "timestamp"])
        wr.writerows(rows)
    print(f"{len(rows)} clipes em {args.out}")


if __name__ == "__main__":
    sys.exit(main())
