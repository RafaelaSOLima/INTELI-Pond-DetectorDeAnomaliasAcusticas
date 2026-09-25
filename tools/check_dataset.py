#!/usr/bin/env python3
"""Verifica o dataset: formato de cada WAV, contagem por classe/pessoa e avisos.

    python3 tools/check_dataset.py [--root dataset/raw]

O manifest.csv é a fonte dos metadados; arquivos apagados (regravados) são
ignorados e WAVs sem linha no manifest são reportados.
"""
import argparse
import collections
import csv
import os
import sys
import wave

LABELS = ["ball", "cat", "dog", "unknown", "noise"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="dataset/raw")
    args = ap.parse_args()

    mpath = os.path.join(args.root, "manifest.csv")
    rows = {}
    if os.path.exists(mpath):
        with open(mpath) as f:
            for r in csv.DictReader(f):
                rows[r["file"]] = r  # a última linha para o mesmo arquivo vale

    problems, counts = [], collections.Counter()
    per_speaker = collections.defaultdict(collections.Counter)
    warned = 0
    on_disk = set()
    for label in sorted(os.listdir(args.root)):
        d = os.path.join(args.root, label)
        if not os.path.isdir(d):
            continue
        if label not in LABELS:
            problems.append(f"pasta inesperada: {d}")
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".wav"):
                continue
            rel = f"{label}/{fn}"
            on_disk.add(rel)
            path = os.path.join(d, fn)
            try:
                with wave.open(path) as w:
                    sr, ch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
            except Exception as e:  # noqa: BLE001
                problems.append(f"{rel}: não abre ({e})")
                continue
            if (sr, ch, sw) != (16000, 1, 2):
                problems.append(f"{rel}: formato {sr} Hz, {ch} canal(is), {8 * sw} bits (esperado 16000/1/16)")
            if not 0.9 * 16000 <= n <= 2.1 * 16000:
                problems.append(f"{rel}: duração {n / 16000:.2f} s")
            r = rows.get(rel)
            if r is None:
                problems.append(f"{rel}: sem linha no manifest.csv")
                spk = fn.split("_")[0]
            else:
                spk = r["speaker"]
                if r["label"] != label:
                    problems.append(f"{rel}: manifest diz label={r['label']}")
                if r.get("warnings"):
                    warned += 1
            counts[label] += 1
            per_speaker[spk][label] += 1

    print(f"\nTotal: {sum(counts.values())} clipes")
    print("Por classe: " + ", ".join(f"{l}={counts[l]}" for l in LABELS))
    print(f"\n{'pessoa':8s}" + "".join(f"{l:>9s}" for l in LABELS))
    for spk in sorted(per_speaker):
        print(f"{spk:8s}" + "".join(f"{per_speaker[spk][l]:9d}" for l in LABELS))
    speakers = [s for s in per_speaker if s != "bg"]
    print(f"\nPessoas (sem 'bg'): {len(speakers)}")
    if warned:
        print(f"Clipes com aviso de qualidade no momento da gravação: {warned} (ouça alguns)")
    if len(speakers) < 6:
        print("AVISO: com menos de ~6 pessoas a divisão treino/validação/teste por pessoa fica frágil.")
    if problems:
        print(f"\n{len(problems)} problema(s):")
        for p in problems[:50]:
            print("  - " + p)
        sys.exit(1)
    print("\nOK: nenhum problema de formato.")


if __name__ == "__main__":
    main()
