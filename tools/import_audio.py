#!/usr/bin/env python3
"""Importa uma gravação LONGA de celular/WhatsApp e corta em clipes de 1,5 s.

A pessoa grava um áudio repetindo UMA palavra várias vezes com ~1 s de pausa
entre as repetições (ex.: "cat ... cat ... cat ..."). Este script:
  1) converte qualquer formato (m4a, ogg/opus, mp3, wav) para 16 kHz mono 16 bits (ffmpeg);
  2) encontra cada repetição pela energia (RMS por frame);
  3) salva um clipe de 1,5 s centrado em cada repetição, com o mesmo padrão de
     nomes e o mesmo manifest.csv do record_session.py.

    python3 tools/import_audio.py audio.ogg --speaker s11 --word cat
    python3 tools/import_audio.py outras.m4a --speaker s11 --word banana --label unknown
    python3 tools/import_audio.py ambiente.m4a --speaker bg --word ambiente --label noise --noise-chop

Requer ffmpeg (sudo apt install ffmpeg).
"""
import argparse
import csv
import datetime as dt
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from record_session import SR, TARGETS, next_take, quality, save_wav  # noqa: E402

CLIP = SR * 3 // 2  # 1,5 s


def load_any(path):
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "s16le", "-"],
        capture_output=True, check=True,
    ).stdout
    return np.frombuffer(out, dtype="<i2").astype(np.int16)


def find_utterances(x, frame=256, min_gap_s=0.25, min_len_s=0.12):
    nf = len(x) // frame
    fr = x[: nf * frame].astype(np.float64).reshape(nf, frame)
    db = 10 * np.log10(np.mean(fr**2, axis=1) + 1e-9)
    floor, top = np.percentile(db, 15), np.percentile(db, 99)
    thr = floor + 0.35 * (top - floor)
    active = db > thr
    segs, start = [], None
    for i, a in enumerate(active):
        if a and start is None:
            start = i
        if not a and start is not None:
            segs.append([start, i])
            start = None
    if start is not None:
        segs.append([start, nf])
    gap = int(min_gap_s * SR / frame)
    merged = []
    for s in segs:  # junta pedaços da mesma palavra separados por pausas curtas
        if merged and s[0] - merged[-1][1] <= gap:
            merged[-1][1] = s[1]
        else:
            merged.append(s)
    minlen = int(min_len_s * SR / frame)
    return [(a * frame, b * frame) for a, b in merged if b - a >= minlen]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("--speaker", required=True)
    ap.add_argument("--word", required=True)
    ap.add_argument("--label", default=None, help="padrão: a própria palavra se for ball/cat/dog")
    ap.add_argument("--device", default="phone")
    ap.add_argument("--env", default="desconhecido")
    ap.add_argument("--noise-chop", action="store_true", help="corta o arquivo inteiro em clipes (ruído)")
    ap.add_argument("--out", default="dataset/raw")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    label = args.label or (args.word if args.word in TARGETS else None)
    if label is None:
        sys.exit("informe --label (unknown ou noise) para palavras fora de ball/cat/dog")

    x = load_any(args.input)
    if args.noise_chop:
        clips = [x[i : i + CLIP] for i in range(0, len(x) - CLIP + 1, CLIP)]
    else:
        clips = []
        for a, b in find_utterances(x):
            c = (a + b) // 2
            s = max(0, min(len(x) - CLIP, c - CLIP // 2))
            if b - a > SR * 1.2:
                print(f"  ignorado trecho longo de {(b - a) / SR:.1f} s em {a / SR:.1f} s (fala contínua?)")
                continue
            clips.append(x[s : s + CLIP])
    print(f"{args.input}: {len(x) / SR:.1f} s -> {len(clips)} clipes")
    if args.dry_run or not clips:
        return

    folder = os.path.join(args.out, label)
    prefix = f"{args.speaker}_{args.word}_{args.device}_"
    manifest = os.path.join(args.out, "manifest.csv")
    new = not os.path.exists(manifest)
    os.makedirs(folder, exist_ok=True)
    with open(manifest, "a", newline="") as mf:
        wr = csv.writer(mf)
        if new:
            wr.writerow(["file", "speaker", "label", "word", "device", "env", "distance",
                         "peak_dbfs", "rms_dbfs", "warnings", "timestamp"])
        for c in clips:
            path = os.path.join(folder, f"{prefix}{next_take(folder, prefix):03d}.wav")
            save_wav(path, c)
            peak, rms, warns = quality(c, expect_speech=label != "noise")
            wr.writerow([os.path.relpath(path, args.out), args.speaker, label, args.word, args.device,
                         args.env, "?", f"{peak:.1f}", f"{rms:.1f}", "|".join(warns),
                         dt.datetime.now().isoformat(timespec="seconds")])
    print(f"salvos em {folder}")


if __name__ == "__main__":
    main()
