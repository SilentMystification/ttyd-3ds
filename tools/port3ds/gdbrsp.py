"""Minimal GDB remote serial protocol client.

Talks to the GDB stubs of Dolphin (PowerPC, -C Dolphin.General.GDBPort=N)
and Azahar (ARM11, -g N). Supports what the test tools need: stop reason,
memory read and write, software breakpoints, continue and interrupt.
"""

import socket
import time


class RSPError(RuntimeError):
    pass


class RSP:
    def __init__(self, host, port, connect_timeout=30.0, timeout=60.0):
        deadline = time.monotonic() + connect_timeout
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=timeout)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise RSPError(f"no GDB stub on {host}:{port}")
                time.sleep(0.25)
        self.buf = b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _byte(self):
        while not self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RSPError("GDB stub closed the connection")
            self.buf = chunk
        b, self.buf = self.buf[:1], self.buf[1:]
        return b

    def send(self, payload):
        cs = sum(payload.encode()) & 0xFF
        self.sock.sendall(f"${payload}#{cs:02x}".encode())

    def recv(self):
        while True:
            c = self._byte()
            if c == b"$":
                break
        body = b""
        while True:
            c = self._byte()
            if c == b"#":
                break
            body += c
        self._byte()
        self._byte()
        self.sock.sendall(b"+")
        return body.decode("latin-1")

    def command(self, payload):
        self.send(payload)
        reply = self.recv()
        if reply.startswith("E") and len(reply) == 3:
            raise RSPError(f"{payload[:20]!r} -> {reply}")
        return reply

    def stop_reason(self):
        return self.command("?")

    def read(self, addr, size):
        out = b""
        while size > 0:
            n = min(size, 0x400)
            out += bytes.fromhex(self.command(f"m{addr:x},{n:x}"))
            addr += n
            size -= n
        return out

    def write(self, addr, data):
        reply = self.command(f"M{addr:x},{len(data):x}:{data.hex()}")
        if reply != "OK":
            raise RSPError(f"memory write at 0x{addr:x} failed: {reply}")

    def set_breakpoint(self, addr, kind=4):
        reply = self.command(f"Z0,{addr:x},{kind}")
        if reply != "OK":
            raise RSPError(f"breakpoint at 0x{addr:x} refused: {reply!r}")

    def clear_breakpoint(self, addr, kind=4):
        self.command(f"z0,{addr:x},{kind}")

    def cont(self):
        """Continues and waits for the next stop. Returns the stop packet."""
        self.send("c")
        return self.recv()

    def interrupt(self):
        self.sock.sendall(b"\x03")
        return self.recv()
