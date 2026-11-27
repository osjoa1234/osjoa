import socket
import sys
import threading

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7000


def serve(conn, addr):
    total = 0
    while True:
        data = conn.recv(4096)
        if not data:
            break
        total += len(data)
        conn.sendall(data)
    print("echo_server: %s:%d EOF after %d bytes" % (addr[0], addr[1], total), flush=True)
    conn.close()


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", PORT))
srv.listen(4)
print("echo_server: listening on 0.0.0.0:%d" % PORT, flush=True)

while True:
    conn, addr = srv.accept()
    print("echo_server: accepted %s:%d" % addr, flush=True)
    threading.Thread(target=serve, args=(conn, addr), daemon=True).start()
