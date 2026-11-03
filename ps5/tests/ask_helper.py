#!/usr/bin/env python3
# Media Player Classic PS5 host tests (1.5.4): one raw request to the helper on 127.0.0.1:<port>, as the app sends it
# (ProsperoJailbreak.h's Request: magic, cmd, pid, ret, msg1[0x500], msg2[0x500]).
#   ask_helper.py <port> <cmd> <pid> [msg1] [msg2]
# Prints "ret <ret> <msg1>" for a whole reply, "closed <n>" when the helper closed the connection after n bytes.
import socket
import struct
import sys

MAGIC = 0xDEADBEEF
MSG = 0x500
SIZE = 16 + 2 * MSG


def field(text):
    data = text.encode("utf-8")[: MSG - 1]
    return data + b"\0" * (MSG - len(data))


def main():
    port, cmd, pid = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    msg1 = sys.argv[4] if len(sys.argv) > 4 else ""
    msg2 = sys.argv[5] if len(sys.argv) > 5 else ""
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall(struct.pack("<Iiii", MAGIC, cmd, pid, -1337) + field(msg1) + field(msg2))
    data = b""
    while len(data) < SIZE:
        part = s.recv(SIZE - len(data))
        if not part:
            break
        data += part
    s.close()
    if len(data) < SIZE:
        print("closed %d" % len(data))
        return
    magic, rcmd, rpid, ret = struct.unpack_from("<Iiii", data)
    text = data[16 : 16 + MSG].split(b"\0", 1)[0].decode("utf-8", "replace")
    print("ret %d %s" % (ret, text))


if __name__ == "__main__":
    main()
