#!/usr/bin/env python3
# Media Player Classic PS5 host tests (1.5.1): stand-ins for the helper payload on 127.0.0.1:<port>.
#   fake_helper.py <port> old            a 1.4 / 1.5 helper: unlocks (cmd 5), closes the connection on cmd 7
#   fake_helper.py <port> silent         a wedged helper: takes the request, never answers
#   fake_helper.py <port> place <file>   unlocks, and answers cmd 7 with <file>'s text (where the app is)
import socket
import struct
import sys
import threading
import time

MAGIC = 0xDEADBEEF
SIZE = 16 + 0x500 + 0x500


def recv_all(c, n):
    data = b""
    while len(data) < n:
        part = c.recv(n - len(data))
        if not part:
            break
        data += part
    return data


def serve(c, mode, extra):
    try:
        req = recv_all(c, SIZE)
        if len(req) != SIZE:
            return
        magic, cmd, pid, ret = struct.unpack_from("<IiiI", req)
        if mode == "silent":
            time.sleep(30)
            return
        if cmd == 5:
            c.sendall(struct.pack("<Iiii", MAGIC, cmd, pid, 0) + req[16:])
        elif cmd == 7 and mode == "place":
            text = open(extra, "rb").read()
            c.sendall(struct.pack("<Iiii", MAGIC, cmd, pid, len(text)) + req[16:] + text)
        # anything else (cmd 7 to an old helper): closed without a reply
    finally:
        c.close()


def main():
    port, mode = int(sys.argv[1]), sys.argv[2]
    extra = sys.argv[3] if len(sys.argv) > 3 else ""
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(8)
    print("listening on 127.0.0.1:%d (%s)" % (port, mode), flush=True)
    while True:
        c, _ = s.accept()
        threading.Thread(target=serve, args=(c, mode, extra), daemon=True).start()


if __name__ == "__main__":
    main()
