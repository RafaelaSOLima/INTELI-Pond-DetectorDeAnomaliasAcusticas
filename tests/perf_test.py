#!/usr/bin/env python3
"""Código de teste: simula eventos acústicos e mede ACURÁCIA e LATÊNCIA.

Cada "evento" é um clipe gravado (ball, cat, dog, palavra fora do vocabulário
ou ruído). O clipe é colocado entre trechos do próprio ruído de fundo e enviado
como um fluxo contínuo de áudio, exatamente como o microfone entregaria.

Modos:
  --port /dev/ttyUSB0   NO ESP32: o áudio é injetado pela USB (comando INJ) e passa
                        por T1 -> T2 -> T3 -> LED do firmware real. Mede latências
                        reais (esp_timer) e o tempo de ida e volta da USB (PING).
  --offline             NO PC: roda o MESMO código C (kws_features.c + kws_model.c
                        compilados com gcc) com a mesma lógica de janela da T2.

    python3 tests/perf_test.py --offline --speakers s05
    python3 tests/perf_test.py --port /dev/ttyUSB0 --speakers s05 --per-class 10
"""
import argparse
import collections
import ctypes
import datetime as dt
import json
import os
import random
import subprocess
import sys
import tempfile
import threading
import time

import numpy as np

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "training"))
import features as F  # noqa: E402
from dataset import load_manifest, pad_clip, read_wav  # noqa: E402

SRC = os.path.join(ROOT, "firmware", "libraries", "kws_common", "src")
DECISIONS = ["ball", "cat", "dog", "unknown"]
CLICK_SAMPLES = 1536  # 96 ms


def truth(label):
    return label if label in ("ball", "cat", "dog") else "unknown"


# ============================================================================ offline (C no PC)
class OfflineDevice:
    def __init__(self):
        tmp = tempfile.mkdtemp()
        so = os.path.join(tmp, "libkws.so")
        subprocess.run(["gcc", "-O2", "-shared", "-fPIC", "-o", so, os.path.join(SRC, "kws_features.c"),
                        os.path.join(SRC, "kws_model.c"), "-lm"], check=True)
        self.lib = ctypes.CDLL(so)
        self.lib.kws_features_init()
        self.lib.kws_features_frame.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.POINTER(ctypes.c_float)]
        self.lib.kws_model_run.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float)]
        hdr = open(os.path.join(SRC, "kws_model_weights.h")).read()
        self.threshold = float(hdr.split("KWS_MODEL_THRESHOLD")[1].split()[0].rstrip("f"))
        self.classes = hdr.split("KWS_MODEL_CLASSES[KWS_MODEL_N_CLASSES] = {")[1].split("}")[0].replace('"', "").replace(" ", "").split(",")

    def run(self, stream):
        """Espelho da lógica da T2 + T3 (task_features.cpp / task_detect.cpp)."""
        vad = F.Vad()
        hist = {}
        pending, search_end, peak_idx, peak_val = False, 0, 0, -1e9
        frame = np.zeros(F.FRAME, np.int16)
        nblocks = len(stream) // F.HOP
        for b in range(nblocks):
            frame[:F.HOP] = frame[F.HOP:]
            frame[F.HOP:] = stream[b * F.HOP:(b + 1) * F.HOP]
            if b == 0:
                continue
            idx = b - 1
            f = np.zeros(F.N_FEAT, np.float32)
            fr = np.ascontiguousarray(frame)
            t0 = time.perf_counter()
            self.lib.kws_features_frame(fr.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)),
                                        f.ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
            hist[idx] = f
            if vad.update(f[13]) and not pending:
                pending, search_end = True, idx + F.PEAK_SEARCH
                first = max(0, idx - F.PRE_ROLL)
                peak_idx = max(range(first, idx + 1), key=lambda k: hist[k][13])
                peak_val = hist[peak_idx][13]
            elif pending and idx <= search_end and f[13] > peak_val:
                peak_idx, peak_val = idx, f[13]
            win_start = max(0, peak_idx - F.PEAK_POS)
            if pending and idx >= search_end and idx >= win_start + F.WIN_FRAMES - 1:
                win = np.ascontiguousarray(np.stack([hist[win_start + k] for k in range(F.WIN_FRAMES)]))
                probs = np.zeros(len(self.classes), np.float32)
                t1 = time.perf_counter()
                self.lib.kws_model_run(win.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                                       probs.ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
                inf_us = (time.perf_counter() - t1) * 1e6
                k = int(probs.argmax())
                word = self.classes[k] if k <= 2 and probs[k] >= self.threshold else "unknown"
                return {"word": word, "conf": float(probs[k]), "ignored": self.classes[k] == "noise",
                        "lat_us": {"inference": inf_us}}
        return None  # o VAD não disparou: nenhuma janela foi classificada


# ============================================================================ ESP32 (injeção)
class Esp32Device:
    def __init__(self, port):
        import serial

        self.ser = serial.Serial(port, 921600, timeout=0.05)
        time.sleep(2.0)
        self.lines = collections.deque()
        self.lock = threading.Lock()
        self.boot = None
        threading.Thread(target=self._reader, daemon=True).start()
        self._cmd("TARGET none")
        time.sleep(0.5)

    def _reader(self):
        buf = b""
        while True:
            buf += self.ser.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    msg = json.loads(line.decode(errors="replace"))
                except ValueError:
                    continue
                msg["_t"] = time.time()
                if msg.get("t") == "boot":
                    self.boot = msg
                with self.lock:
                    self.lines.append(msg)

    def _cmd(self, s):
        self.ser.write((s + "\n").encode())

    def _wait(self, kind, timeout, since=0.0):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for i, m in enumerate(self.lines):
                    if m.get("t") == kind and m["_t"] >= since:
                        del self.lines[i]
                        return m
            time.sleep(0.01)
        return None

    def ping_rtt(self, n=20):
        rtts = []
        for i in range(n):
            t0 = time.time()
            self._cmd(f"PING {i}")
            if self._wait("pong", 1.0, since=t0):
                rtts.append((time.time() - t0) * 1e6)
        return rtts

    def run(self, stream):
        # Espera o sistema voltar a ouvir (fim do LED + cooldown da rodada anterior).
        # Só vale um heartbeat NOVO: um "listening=1" antigo ainda na fila faria o
        # clipe ser enviado durante o cooldown, com o ESP32 propositalmente surdo.
        with self.lock:
            self.lines.clear()
        t_end = time.time() + 5
        while time.time() < t_end:
            t_ask = time.time()
            hb = self._wait("hb", 1.5, since=t_ask)
            if hb and hb.get("listening") == 1:
                break
        time.sleep(0.2)  # margem: o VAD reaprende o piso de ruído
        with self.lock:
            self.lines.clear()
        data = stream.astype("<i2").tobytes()
        t0 = time.time()
        self._cmd(f"INJ {len(data)}")
        # envia ~10% mais rápido que o tempo real (o firmware freia pelo stream buffer)
        chunk, rate = 1024, 32000 * 1.1
        for i in range(0, len(data), chunk):
            self.ser.write(data[i:i + chunk])
            ahead = (i + chunk) / rate - (time.time() - t0)
            if ahead > 0.15:
                time.sleep(ahead - 0.15)
        # o firmware responde "result" (fala classificada) ou "ignored" (classe noise: sem LED)
        end = time.time() + 3.0
        while time.time() < end:
            for kind in ("result", "ignored"):
                m = self._wait(kind, 0.05, since=t0)
                if m:
                    if kind == "ignored":
                        m["word"], m["ignored"] = "unknown", True
                    return m
        return None


# ============================================================================ relatório
def summarize(rows, rtts, mode, out_dir):
    cm = np.zeros((4, 4), int)
    for r in rows:
        cm[DECISIONS.index(r["truth"]), DECISIONS.index(r["pred"])] += 1
    acc = np.trace(cm) / max(cm.sum(), 1)
    no_det = sum(r["no_detection"] for r in rows)
    ignored = sum(bool(r.get("ignored")) for r in rows)
    lat = collections.defaultdict(list)
    for r in rows:
        for k, v in (r.get("lat_us") or {}).items():
            lat[k].append(v)
    if rtts:
        lat["usb_round_trip"] = rtts
    L = [f"# Teste de performance ({mode}) — {dt.datetime.now().isoformat(timespec='seconds')}\n",
         f"- Eventos simulados: {len(rows)} | acurácia (ball/cat/dog/unknown): **{acc:.3f}**",
         f"- Eventos sem detecção de fala (VAD não disparou, contados como unknown): {no_det}",
         f"- Eventos classificados como ruído e ignorados (sem LED, contados como unknown): {ignored}\n",
         "| real \\ previsto | " + " | ".join(DECISIONS) + " |", "|---|---|---|---|---|"]
    for i, d in enumerate(DECISIONS):
        L.append(f"| **{d}** | " + " | ".join(str(v) for v in cm[i]) + " |")
    L += ["\n## Latência (µs)\n", "| etapa | n | média | mediana | p95 | máx |", "|---|---|---|---|---|---|"]
    for k, v in lat.items():
        v = np.array(v, float)
        L.append(f"| {k} | {len(v)} | {v.mean():.0f} | {np.median(v):.0f} | {np.percentile(v, 95):.0f} | {v.max():.0f} |")
    text = "\n".join(L)
    print("\n" + text)
    os.makedirs(out_dir, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    with open(os.path.join(out_dir, f"perf_{mode}_{stamp}.md"), "w") as f:
        f.write(text + "\n")
    with open(os.path.join(out_dir, f"perf_{mode}_{stamp}.json"), "w") as f:
        json.dump({"rows": rows, "usb_rtt_us": rtts}, f, indent=1)
    print(f"\nsalvo em {out_dir}/perf_{mode}_{stamp}.md/.json")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--offline", action="store_true")
    ap.add_argument("--root", default=os.path.join(ROOT, "dataset", "raw"))
    ap.add_argument("--speakers", nargs="*", help="pessoas de teste (padrão: todas)")
    ap.add_argument("--per-class", type=int, default=0, help="limite de clipes por classe (0 = todos)")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "results"))
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    if not args.offline and not args.port:
        sys.exit("use --offline ou --port /dev/ttyUSB0")

    clips = [c for c in load_manifest(args.root) if c["speaker"] not in ("bg", "teste")]
    if args.speakers:
        clips = [c for c in clips if c["speaker"] in args.speakers]
    random.Random(args.seed).shuffle(clips)
    if args.per_class:
        cnt = collections.Counter()
        sel = []
        for c in clips:
            if cnt[c["label"]] < args.per_class:
                sel.append(c)
                cnt[c["label"]] += 1
        clips = sel
    print(f"{len(clips)} eventos ({collections.Counter(c['label'] for c in clips)})")

    dev = OfflineDevice() if args.offline else Esp32Device(args.port)
    if not args.offline and dev.boot and dev.boot.get("placeholder"):
        print("ATENÇÃO: o firmware está com o modelo PLACEHOLDER (sintético)")
    rtts = [] if args.offline else dev.ping_rtt()
    rows = []
    for i, c in enumerate(clips):
        # Os primeiros ~96 ms das gravações contêm o clique da tecla Enter usada na
        # coleta (não existe no uso real); são descartados antes de simular o stream.
        res = dev.run(pad_clip(read_wav(c["path"])[CLICK_SAMPLES:]))
        pred = res["word"] if res else "unknown"
        rows.append({"file": c["file"], "label": c["label"], "truth": truth(c["label"]), "pred": pred,
                     "conf": res.get("conf") if res else None, "no_detection": res is None,
                     "ignored": bool(res.get("ignored")) if res else False,
                     "lat_us": res.get("lat_us") if res else None})
        mark = "OK " if pred == truth(c["label"]) else "ERR"
        print(f"[{i + 1}/{len(clips)}] {mark} {c['file']:40s} -> {pred}" + (f" ({res['conf']:.2f})" if res else " (sem detecção)"))
    summarize(rows, rtts, "offline" if args.offline else "esp32", args.out)


if __name__ == "__main__":
    main()
