#!/usr/bin/env python3
"""Install a CIA on the console through FBI's "Receive URLs over the network".

    N3DS_IP=192.168.1.50 tools/port3ds/fbi_install.py [--cia build/3ds/game/ttyd.cia] [--port 8080]

On the console: FBI > Remote Install > Receive URLs over the network. This
serves a copy of the CIA from a temporary folder over HTTP on this PC's
address toward the console, sends FBI the URL (a 4-byte big-endian length,
then the URL, on TCP 5000) and waits until FBI closes the connection (a
reset counts: FBI does that when it finishes). It then says whether the
file was requested in full. Python 3, standard library only.
"""
import argparse
import functools
import http.server
import os
import shutil
import socket
import struct
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Handler(http.server.SimpleHTTPRequestHandler):
    sent = 0
    requests = []

    def copyfile(self, source, outputfile):  # counts the bytes served
        while True:
            buf = source.read(65536)
            if not buf:
                break
            outputfile.write(buf)
            Handler.sent += len(buf)

    def log_message(self, fmt, *args):
        line = "%s %s" % (self.address_string(), fmt % args)
        Handler.requests.append(line)
        print("http:", line, flush=True)


def lan_address(console, port):
    """This PC's address on the route to the console (no packet is sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((console, port))
        return s.getsockname()[0]
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default=os.environ.get("N3DS_IP"), help="the console (default $N3DS_IP)")
    ap.add_argument("--cia", type=Path, default=ROOT / "build/3ds/game/ttyd.cia")
    ap.add_argument("--port", type=int, default=8080, help="HTTP port on this PC")
    ap.add_argument("--fbi-port", type=int, default=5000, help="FBI's receive port on the console")
    ap.add_argument("--timeout", type=float, default=600.0, help="seconds to wait for FBI to finish")
    args = ap.parse_args()
    if not args.ip:
        print("fbi_install: set N3DS_IP or pass --ip", file=sys.stderr)
        return 2
    if not args.cia.is_file():
        print(f"fbi_install: {args.cia} not found", file=sys.stderr)
        return 2

    host = lan_address(args.ip, args.fbi_port)
    with tempfile.TemporaryDirectory(prefix="fbi_install_") as tmp:
        name = args.cia.name
        shutil.copyfile(args.cia, Path(tmp) / name)
        size = (Path(tmp) / name).stat().st_size
        httpd = http.server.ThreadingHTTPServer((host, args.port), functools.partial(Handler, directory=tmp))
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        url = f"http://{host}:{args.port}/{name}"
        print(f"fbi_install: serving {url} ({size} bytes)", flush=True)

        try:
            c = socket.create_connection((args.ip, args.fbi_port), timeout=30)
        except OSError as e:
            print(f"fbi_install: FAIL: cannot reach FBI at {args.ip}:{args.fbi_port} ({e}); is "
                  "Remote Install > Receive URLs over the network open?")
            httpd.shutdown()
            return 1
        payload = url.encode()
        c.sendall(struct.pack(">I", len(payload)) + payload)
        print("fbi_install: URL sent; waiting for FBI to finish", flush=True)
        c.settimeout(args.timeout)
        try:
            while c.recv(1):
                pass
        except ConnectionResetError:
            pass  # FBI closes with a reset when it is done
        except socket.timeout:
            print("fbi_install: timeout waiting for FBI")
        c.close()
        httpd.shutdown()

    full = Handler.sent >= size
    print(f"fbi_install: served {Handler.sent} of {size} bytes in {len(Handler.requests)} request(s): "
          + ("the file was fully requested" if full else "NOT fully requested"))
    return 0 if full else 1


if __name__ == "__main__":
    sys.exit(main())
