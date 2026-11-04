#!/usr/bin/env python3
import socket
import sys
import time

HELLO_HOST = b"hello host"


def serve(port, seconds):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", port))
    sock.settimeout(1.0)
    print(f"peer: listening on 127.0.0.1:{port}", flush=True)

    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        print(f"peer: rx from {addr[0]}:{addr[1]} ({len(data)} bytes)", flush=True)
        sock.sendto(data, addr)
        print(f"peer: tx to {addr[0]}:{addr[1]} ({len(data)} bytes)", flush=True)


def client(port, seconds):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)

    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        sock.sendto(HELLO_HOST, ("127.0.0.1", port))
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        print(f"peer: client rx from {addr[0]}:{addr[1]} ({len(data)} bytes)", flush=True)
        return 0 if data == HELLO_HOST else 1

    print("peer: client no reply", flush=True)
    return 1


def main(argv):
    if len(argv) < 4 or argv[1] not in ("serve", "client"):
        sys.stderr.write("usage: udp_peer.py serve|client <port> <seconds>\n")
        return 2

    port = int(argv[2])
    seconds = float(argv[3])

    if argv[1] == "serve":
        serve(port, seconds)
        return 0
    return client(port, seconds)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
