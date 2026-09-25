#!/usr/bin/env python3
"""Console serial mínimo: digite um comando (ex.: LEVEL, CHAN, LEDS, PING) e Enter.

    python3 tools/serial_console.py --port /dev/ttyUSB0
Ctrl+C para sair.
"""
import argparse
import sys
import threading

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()
    ser = serial.Serial(args.port, args.baud, timeout=0.1)

    def reader():
        while True:
            data = ser.read(4096)
            if data:
                sys.stdout.write(data.decode(errors="replace"))
                sys.stdout.flush()

    threading.Thread(target=reader, daemon=True).start()
    try:
        for line in sys.stdin:
            ser.write(line.strip().encode() + b"\n")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
