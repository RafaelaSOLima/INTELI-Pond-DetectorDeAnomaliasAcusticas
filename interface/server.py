#!/usr/bin/env python3
"""Ponte USB <-> navegador para a interface (alternativa à Web Serial).

Serve interface/index.html e repassa, sem processar nada:
  ESP32 -> página: cada linha JSON da serial vira um evento SSE em /events
  página -> ESP32: POST /send com uma linha de texto (ex.: "TARGET cat")

    python3 interface/server.py --port /dev/ttyUSB0
    (abra http://localhost:8000 em qualquer navegador)

Só usa a biblioteca padrão + pyserial. Nenhum áudio passa por aqui.
"""
import argparse
import http.server
import os
import queue
import threading
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
clients = []           # uma fila por página conectada
clients_lock = threading.Lock()
ser = None
ser_lock = threading.Lock()


def serial_reader(port, baud):
    """Lê linhas da serial e distribui para todas as páginas. Reabre se o cabo sair."""
    global ser
    buf = b""
    while True:
        try:
            if ser is None:
                ser = serial.Serial(port, baud, timeout=0.1)
                print(f"[ponte] conectado em {port}")
                broadcast('{"t":"bridge","connected":true}')
            data = ser.read(4096)
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="replace").strip()
                if text.startswith("{"):
                    broadcast(text)
        except (serial.SerialException, OSError) as e:
            print(f"[ponte] serial indisponível ({e}); tentando de novo em 1 s")
            broadcast('{"t":"bridge","connected":false}')
            try:
                if ser:
                    ser.close()
            except Exception:  # noqa: BLE001
                pass
            ser = None
            buf = b""
            time.sleep(1)


def broadcast(line):
    with clients_lock:
        for q in clients:
            try:
                q.put_nowait(line)
            except queue.Full:
                pass


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):  # silencia o log por requisição
        pass

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body = open(os.path.join(HERE, "index.html"), "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/events":
            q = queue.Queue(maxsize=500)
            with clients_lock:
                clients.append(q)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            try:
                self.wfile.write(f'data: {{"t":"bridge","connected":{str(ser is not None).lower()}}}\n\n'.encode())
                self.wfile.flush()
                while True:
                    try:
                        line = q.get(timeout=5)
                        self.wfile.write(f"data: {line}\n\n".encode())
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with clients_lock:
                    clients.remove(q)
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != "/send":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", 0))
        line = self.rfile.read(n).decode().strip()
        ok = False
        with ser_lock:
            if ser is not None:
                try:
                    ser.write((line + "\n").encode())
                    ok = True
                except (serial.SerialException, OSError):
                    ok = False
        self.send_response(200 if ok else 503)
        self.end_headers()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--http", type=int, default=8000)
    args = ap.parse_args()
    threading.Thread(target=serial_reader, args=(args.port, args.baud), daemon=True).start()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", args.http), Handler)
    print(f"[ponte] abra http://localhost:{args.http} no navegador (Ctrl+C para sair)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
