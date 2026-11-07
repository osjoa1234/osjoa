#!/usr/bin/env python3
import socket

HELLO_HOST = b"hello host"


def serve(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", port))
    print(f"peer: listening on 127.0.0.1:{port}", flush=True)

    data, addr = sock.recvfrom(2048)
    print(f"peer: rx from {addr[0]}:{addr[1]} ({len(data)} bytes) {data!r}", flush=True)
    sock.close()


def client(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(HELLO_HOST, ("127.0.0.1", port))

    data, addr = sock.recvfrom(2048)
    print(f"peer: client rx from {addr[0]}:{addr[1]} ({len(data)} bytes) {data!r}", flush=True)


serve(5000)
client(5001)
