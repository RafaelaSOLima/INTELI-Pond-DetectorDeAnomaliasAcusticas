#!/usr/bin/env python3
"""Simula o firmware recorder numa porta serial virtual (pty), para testar
tools/record_session.py sem hardware. Imprime o caminho da porta criada.

    python3 tests/fake_recorder.py   ->  "PORT /dev/pts/N"
"""
import os
import pty
import sys

import numpy as np

SR = 16000


def fake_clip(n, rng):
    """Ruído de fundo + uma 'palavra' sintética (burst harmônico) no meio."""
    x = rng.normal(0, 60, n)
    L = int(0.4 * SR)
    s = (n - L) // 2
    t = np.arange(L) / SR
    f0 = rng.uniform(120, 250)
    word = sum(np.sin(2 * np.pi * f0 * k * t) / k for k in range(1, 6)) * np.hanning(L) * 6000
    x[s : s + L] += word
    return np.clip(x, -32768, 32767).astype("<i2")


def main():
    master, slave = pty.openpty()
    os.system(f"stty -F {os.ttyname(slave)} raw -echo")
    print("PORT", os.ttyname(slave), flush=True)
    rng = np.random.default_rng(0)
    buf = b""
    while True:
        data = os.read(master, 1024)
        if not data:
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            cmd = line.decode().strip()
            if cmd == "PING":
                os.write(master, b"PONG recorder\n")
            elif cmd.startswith("REC"):
                n = SR * int(cmd.split()[1]) // 1000
                x = fake_clip(n, rng)
                os.write(master, f"DATA {n}\n".encode() + x.tobytes() + b"\nEND\n")


if __name__ == "__main__":
    sys.exit(main())
