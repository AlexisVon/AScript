#!/usr/bin/env python3
"""Feed msp_frames.txt -- a real client's own captured bytes -- to a cip listener and print what
came back, so run.sh pins the frames a foreign Logix-style client gets."""
import os
import socket
import sys
import time

addr = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:44838"
host, port = addr.rsplit(":", 1)
frames_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "msp_frames.txt")
frames = [bytes.fromhex(line) for line in open(frames_file) if line.strip()]

sock = None
for _ in range(50):
    try:
        sock = socket.create_connection((host, int(port)), timeout=2)
        break
    except OSError:
        time.sleep(0.1)
if sock is None:
    sys.exit("msp_replay: the listener never came up on " + addr)


def exchange(frame):
    sock.sendall(frame)
    header = b""
    while len(header) < 24:
        header += sock.recv(24 - len(header))
    body = b""
    want = int.from_bytes(header[2:4], "little")
    while len(body) < want:
        body += sock.recv(want - len(body))
    return header + body


for number, frame in enumerate(frames):
    reply = exchange(frame)
    if frame[0] == 0x65:  # RegisterSession
        print("register: %d in, %d out, session=%d" %
              (len(frame), len(reply), int.from_bytes(reply[4:8], "little")))
        continue
    cip = reply[40:]  # the CIP reply: 4 bytes of header, then the packet
    count = int.from_bytes(cip[4:6], "little")
    offset = int.from_bytes(cip[6:8], "little")
    sub = cip[4 + offset:]  # sub-replies are measured from the count word
    print("request %d: %d in, %d out, msp status=0x%02x, %d reply at %d" %
          (number, len(frame), len(reply), cip[2], count, offset))
    print("  sub: service=0x%02x status=0x%02x type=0x%04x data=%s" %
          (sub[0], sub[2], int.from_bytes(sub[4:6], "little"), sub[6:].hex()))

sock.close()
