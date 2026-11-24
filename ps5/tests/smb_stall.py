#!/usr/bin/env python3
"""A share that stops answering, for the host tests (1.7.1, audits B1, C1, D1, D2).

  smb_stall.py hole PORT                 accepts connections and never answers (a NAS asleep, a server gone)
  smb_stall.py proxy PORT TARGET FLAG    passes the bytes to 127.0.0.1:TARGET until the file FLAG exists, then holds
                                         every connection open without passing anything more (a share that stopped
                                         answering in the middle of a film)
"""
import os
import socket
import sys
import threading
import time


def hole(port):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    held = []
    while True:
        c, _ = srv.accept()
        held.append(c)  # (kept open: read nothing, answer nothing)


def pump(src, dst, flag):
    try:
        while True:
            if os.path.exists(flag):
                time.sleep(3600)
            data = src.recv(65536)
            if not data:
                break
            while os.path.exists(flag):
                time.sleep(3600)  # (stalled: the bytes read are never passed on)
            dst.sendall(data)
    except OSError:
        pass


def proxy(port, target, flag):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    while True:
        c, _ = srv.accept()
        try:
            t = socket.create_connection(("127.0.0.1", target))
        except OSError:
            c.close()
            continue
        for a, b in ((c, t), (t, c)):
            threading.Thread(target=pump, args=(a, b, flag), daemon=True).start()


if __name__ == "__main__":
    if sys.argv[1] == "hole":
        hole(int(sys.argv[2]))
    else:
        proxy(int(sys.argv[2]), int(sys.argv[3]), sys.argv[4])
