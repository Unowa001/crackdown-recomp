"""Misbehaving WebSocket endpoints for AP client timeout tests (stdlib only).

  stall   accept TCP and never answer the HTTP upgrade (handshake timeout)
  silent  complete the WebSocket upgrade, then never send a frame (idle timeout)
Each connection is logged with its time so reconnect/backoff can be read off.
Usage: ap_fake_server.py MODE PORT LOG
"""
import base64
import hashlib
import socket
import sys
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def log(path, text):
    with open(path, "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%H:%M:%S')} {text}\n")


def serve(conn, mode, path):
    held = [conn]
    try:
        if mode == "silent":
            request = b""
            while b"\r\n\r\n" not in request:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                request += chunk
            key = next(line.split(b":", 1)[1].strip() for line in request.split(b"\r\n")
                       if line.lower().startswith(b"sec-websocket-key:"))
            accept = base64.b64encode(hashlib.sha1(key + GUID.encode()).digest())
            conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                         b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
            log(path, "upgraded; now silent")
        while True:
            data = conn.recv(4096)
            if not data:
                log(path, "client closed")
                return
            log(path, f"client sent {len(data)} bytes")
    except OSError as e:
        log(path, f"connection error {e}")
    finally:
        held.clear()
        conn.close()


def main():
    mode, port, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    server = socket.create_server(("127.0.0.1", port))
    log(path, f"{mode} listening on {port}")
    while True:
        conn, _ = server.accept()
        log(path, "accepted")
        threading.Thread(target=serve, args=(conn, mode, path), daemon=True).start()


if __name__ == "__main__":
    main()
