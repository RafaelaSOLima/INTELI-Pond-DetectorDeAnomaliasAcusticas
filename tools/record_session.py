#!/usr/bin/env python3
"""Sessão guiada de gravação do dataset (uma pessoa por execução).

Mostra a palavra na tela, grava 1,5 s, confere a qualidade e salva:
    dataset/raw/<label>/<speaker>_<word>_<device>_<take>.wav
e acrescenta uma linha em dataset/raw/manifest.csv.

Fontes de áudio:
    --source esp32  ESP32 + INMP441 com firmware/recorder (PREFERENCIAL: mesmo microfone do produto)
    --source mic    microfone do computador via `arecord` (plano B)

Exemplos:
    python3 tools/record_session.py --speaker s01 --source esp32 --port /dev/ttyUSB0 --env sala
    python3 tools/record_session.py --speaker bg --noise-only 40 --source esp32 --port /dev/ttyUSB0 --env cozinha

Teclas durante a sessão:
    Enter = gravar item   r = regravar o anterior   p = ouvir o anterior
    s = pular item        q = sair (dá para continuar depois; nada é sobrescrito)
"""
import argparse
import csv
import datetime as dt
import os
import random
import shutil
import subprocess
import sys
import time
import wave

import numpy as np

SR = 16000
CLIP_MS = 1500
TARGETS = ["ball", "cat", "dog"]

# Palavras fora do vocabulário. Inclui palavras PARECIDAS com as alvo (o caso
# difícil), palavras comuns em inglês e as traduções em português (a criança
# que fala "gato" deve receber TRY AGAIN).
UNKNOWN_WORDS = [
    # parecidas com ball / cat / dog
    "bowl", "bell", "doll", "call", "fall", "wall", "tall", "bat", "cap", "cut",
    "hat", "cake", "dug", "duck", "dot", "door", "log", "frog",
    # inglês comum
    "apple", "banana", "pineapple", "hello", "yes", "no", "water", "house",
    "tree", "fish", "bird", "sun", "book", "car", "red", "blue", "mom",
    # português
    "bola", "gato", "cachorro", "casa", "oi", "sim", "nao", "agua", "mamae",
]

NOISE_PROMPTS = [
    ("silencio", "fique em SILÊNCIO"),
    ("silencio", "fique em SILÊNCIO"),
    ("palma", "bata UMA PALMA"),
    ("tosse", "TUSSA ou pigarreie"),
    ("respiro", "RESPIRE forte perto do microfone"),
    ("mesa", "BATA de leve na mesa"),
    ("frase", "fale uma frase curta em PORTUGUÊS"),
]

DISTANCES = [("perto", "~15 cm"), ("medio", "~40 cm"), ("longe", "~80 cm")]


# --------------------------------------------------------------------------- fontes
class Esp32Source:
    device = "esp32"

    def __init__(self, port, baud=921600):
        import serial  # pyserial

        self.ser = serial.Serial(port, baud, timeout=3)
        time.sleep(2.0)  # abrir a porta reinicia o ESP32 em muitas placas
        self.ser.reset_input_buffer()
        self.ser.write(b"PING\n")
        deadline = time.time() + 5
        while time.time() < deadline:
            line = self.ser.readline().decode(errors="replace").strip()
            if line == "PONG recorder":
                return
        raise RuntimeError("ESP32 não respondeu PONG. Firmware recorder gravado? Porta correta?")

    def record(self, ms):
        self.ser.reset_input_buffer()
        self.ser.write(f"REC {ms}\n".encode())
        deadline = time.time() + ms / 1000 + 5
        n = None
        while time.time() < deadline:
            line = self.ser.readline().decode(errors="replace").strip()
            if line.startswith("DATA "):
                n = int(line.split()[1])
                break
        if n is None:
            raise RuntimeError("não recebi DATA do ESP32")
        raw = self.ser.read(n * 2)
        if len(raw) != n * 2:
            raise RuntimeError(f"recebi {len(raw)} de {n * 2} bytes")
        tail = self.ser.read(5)
        if tail != b"\nEND\n":
            raise RuntimeError(f"fim de pacote inesperado: {tail!r}")
        return np.frombuffer(raw, dtype="<i2").copy()


class MicSource:
    device = "mic"

    def __init__(self, alsa_device=None):
        if not shutil.which("arecord"):
            raise RuntimeError("arecord não encontrado (sudo apt install alsa-utils)")
        self.alsa_device = alsa_device

    def record(self, ms):
        n = SR * ms // 1000
        cmd = ["arecord", "-q", "-f", "S16_LE", "-r", str(SR), "-c", "1", "-t", "raw", "-s", str(n)]
        if self.alsa_device:
            cmd += ["-D", self.alsa_device]
        out = subprocess.run(cmd, capture_output=True, check=True).stdout
        return np.frombuffer(out, dtype="<i2").copy()


# --------------------------------------------------------------------------- qualidade
def quality(x, expect_speech):
    """Retorna (peak_dbfs, rms_dbfs, avisos)."""
    xf = x.astype(np.float64)
    peak = np.max(np.abs(xf)) if len(xf) else 0
    rms = np.sqrt(np.mean(xf**2)) if len(xf) else 0
    db = lambda v: 20 * np.log10(max(v, 1e-9) / 32768)
    warns = []
    if len(x) < SR * CLIP_MS // 1000 * 0.95:
        warns.append("clipe curto")
    if np.mean(np.abs(xf) >= 32767) > 0.001:
        warns.append("SATURADO (fale mais longe/baixo)")
    frame = 256
    nf = len(xf) // frame
    if nf > 4:
        fr = xf[: nf * frame].reshape(nf, frame)
        fdb = 10 * np.log10(np.mean(fr**2, axis=1) + 1e-9)
        floor, top = np.percentile(fdb, 10), np.max(fdb)
        if expect_speech:
            if top - floor < 12:
                warns.append("NÃO detectei fala (fale mais alto/perto)")
            else:
                loud = fdb > floor + 0.5 * (top - floor)
                if loud[:3].any() or loud[-3:].any():
                    warns.append("palavra pode estar CORTADA no início/fim")
    return db(peak), db(rms), warns


def save_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(x.astype("<i2").tobytes())


def next_take(folder, prefix):
    os.makedirs(folder, exist_ok=True)
    takes = [
        int(f[len(prefix) : -4])
        for f in os.listdir(folder)
        if f.startswith(prefix) and f.endswith(".wav") and f[len(prefix) : -4].isdigit()
    ]
    return max(takes, default=0) + 1


# --------------------------------------------------------------------------- sessão
def build_plan(args, rng):
    items = []
    if args.noise_only:
        for _ in range(args.noise_only):
            items.append(("noise", "ambiente", "fique em SILÊNCIO (ruído do ambiente)"))
        return items
    for w in TARGETS:
        items += [(w, w, w.upper()) for _ in range(args.reps)]
    for w in rng.sample(UNKNOWN_WORDS, min(args.unknown, len(UNKNOWN_WORDS))):
        items.append(("unknown", w, w.upper()))
    for i in range(args.noise):
        tag, txt = NOISE_PROMPTS[i % len(NOISE_PROMPTS)]
        items.append(("noise", tag, txt))
    rng.shuffle(items)  # ordem aleatória: evita que cansaço/ritmo fiquem ligados a uma classe
    return items


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--speaker", required=True, help="ID anônimo: s01, s02, ... (bg = só ambiente)")
    ap.add_argument("--source", choices=["esp32", "mic"], default="esp32")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--alsa-device", default=None)
    ap.add_argument("--env", default="sala", help="ambiente: sala, quarto, cozinha, externo...")
    ap.add_argument("--reps", type=int, default=20, help="repetições de cada palavra-alvo")
    ap.add_argument("--unknown", type=int, default=30, help="palavras fora do vocabulário")
    ap.add_argument("--noise", type=int, default=10, help="clipes de ruído/silêncio")
    ap.add_argument("--noise-only", type=int, default=0, help="grava só N clipes de ambiente")
    ap.add_argument("--out", default="dataset/raw")
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()

    rng = random.Random(args.seed if args.seed is not None else args.speaker)
    src = Esp32Source(args.port) if args.source == "esp32" else MicSource(args.alsa_device)
    plan = build_plan(args, rng)
    manifest = os.path.join(args.out, "manifest.csv")
    os.makedirs(args.out, exist_ok=True)
    new_manifest = not os.path.exists(manifest)
    mf = open(manifest, "a", newline="")
    wr = csv.writer(mf)
    if new_manifest:
        wr.writerow(["file", "speaker", "label", "word", "device", "env", "distance",
                     "peak_dbfs", "rms_dbfs", "warnings", "timestamp"])

    last = None  # (path, label, word, prompt, dist)
    i = 0
    print(f"\n{len(plan)} itens. Dica: alterne a distância como indicado e varie velocidade/volume.\n")
    while i < len(plan):
        label, word, prompt = plan[i]
        dist, dist_txt = rng.choice(DISTANCES) if label != "noise" or word == "frase" else ("medio", "~40 cm")
        print("=" * 60)
        print(f"[{i + 1}/{len(plan)}]  distância: {dist.upper()} ({dist_txt})")
        print(f"\n        >>>   {prompt}   <<<\n")
        cmd = input("Enter=gravar  r=regravar anterior  p=ouvir anterior  s=pular  q=sair > ").strip().lower()
        if cmd == "q":
            break
        if cmd == "s":
            i += 1
            continue
        if cmd == "p" and last:
            subprocess.run(["aplay", "-q", last[0]])
            continue
        if cmd == "r" and last:
            os.remove(last[0])
            print(f"apagado {last[0]} — regravando '{last[3]}'")
            plan.insert(i, last[1:4])
            last = None
            continue

        print("🔴 GRAVANDO — FALE AGORA!" if label != "noise" else "🔴 GRAVANDO...")
        x = src.record(CLIP_MS)
        peak, rms, warns = quality(x, expect_speech=label in TARGETS or label == "unknown")
        folder = os.path.join(args.out, label)
        prefix = f"{args.speaker}_{word}_{src.device}_"
        path = os.path.join(folder, f"{prefix}{next_take(folder, prefix):03d}.wav")
        save_wav(path, x)
        wr.writerow([os.path.relpath(path, args.out), args.speaker, label, word, src.device, args.env,
                     dist, f"{peak:.1f}", f"{rms:.1f}", "|".join(warns), dt.datetime.now().isoformat(timespec="seconds")])
        mf.flush()
        status = "OK" if not warns else "ATENÇÃO: " + "; ".join(warns) + "  (tecle r para regravar)"
        print(f"salvo {path}  pico {peak:.1f} dBFS  rms {rms:.1f} dBFS  -> {status}")
        last = (path, label, word, prompt, dist)
        i += 1

    mf.close()
    print("\nSessão encerrada. Rode: python3 tools/check_dataset.py")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\ninterrompido")
        sys.exit(1)
