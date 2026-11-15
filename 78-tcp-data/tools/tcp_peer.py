#!/usr/bin/env python3
import socket
import time

SEND_LEN = 4000
RECV_LEN = 6000


def pattern(n):
    return bytes(ord('A') + i % 26 for i in range(n))


def recv_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            break
        data += chunk
    return data


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


def guest_client_data(conn):
    conn.settimeout(10)
    data = recv_exact(conn, SEND_LEN)
    print(f"peer: tcp rx {len(data)} bytes from guest, pattern {'ok' if data == pattern(SEND_LEN) else 'BAD'}", flush=True)
    conn.sendall(pattern(RECV_LEN))
    print(f"peer: tcp sent {RECV_LEN} bytes to guest", flush=True)


def guest_server_data(conn):
    conn.settimeout(10)
    conn.sendall(b"ping from host")
    echo = recv_exact(conn, len(b"ping from host"))
    print(f"peer: tcp echo from guest {echo!r}", flush=True)


held = [serve(5000), client(5002)]
refused(5003)
guest_client_data(held[0])
guest_server_data(held[1])
time.sleep(2)
for conn in held:
    conn.close()
