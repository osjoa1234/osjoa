#!/usr/bin/env python3
import socket
import time


def serve(port):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", port))
    server.listen(1)
    print(f"peer: tcp listening on 127.0.0.1:{port}", flush=True)

    conn, addr = server.accept()
    print(f"peer: tcp accepted from {addr[0]}:{addr[1]}", flush=True)
    server.close()
    return conn


def client(port):
    conn = socket.create_connection(("127.0.0.1", port))
    print(f"peer: tcp connected to 127.0.0.1:{port}", flush=True)
    return conn


def refused(port):
    conn = socket.create_connection(("127.0.0.1", port))
    conn.settimeout(5)
    print(f"peer: tcp connected to 127.0.0.1:{port} (no listener in guest)", flush=True)

    try:
        data = conn.recv(1)
    except ConnectionResetError:
        print(f"peer: tcp port {port} reset by guest", flush=True)
    except socket.timeout:
        print(f"peer: tcp port {port} still open after 5s", flush=True)
    else:
        print(f"peer: tcp port {port} closed by guest ({data!r})", flush=True)
    conn.close()


held = [serve(5000), client(5002)]
refused(5003)
time.sleep(2)
for conn in held:
    conn.close()
