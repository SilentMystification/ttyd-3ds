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

    # Helpers shared by hang_probe.py and pc_sample.py.

    def thread_ids(self):
        """Thread ids from qfThreadInfo / qsThreadInfo."""
        ids = []
        reply = self.command("qfThreadInfo")
        while reply.startswith("m"):
            ids += [int(t, 16) for t in reply[1:].split(",") if t]
            reply = self.command("qsThreadInfo")
        return ids

    def core_registers(self, tid):
        """r0..r15 of one thread (Hg + g); r13 is sp, r14 lr, r15 pc."""
        self.command(f"Hg{tid:x}")
        regs = self.command("g")
        return [int.from_bytes(bytes.fromhex(regs[i:i + 8]), "little") for i in range(0, 16 * 8, 8)]

    def attach(self, pid):
        """Luma3DS Rosalina: attach by process id (extended-remote). Returns the stop reply."""
        return self.command(f"vAttach;{pid:x}")

    def detach(self):
        self.send("D")
        return self.recv()

    def core_registers_many(self, tids):
        """{tid: r0..r15} for several threads with the Hg/g requests pipelined (one round trip
        per batch instead of two per thread). Threads that cannot be read are left out."""
        for t in tids:
            self.send(f"Hg{t:x}")
            self.send("g")
        out = {}
        for t in tids:
            hg, regs = self.recv(), self.recv()
            if hg == "OK" and len(regs) >= 128 and not (regs.startswith("E") and len(regs) == 3):
                out[t] = [int.from_bytes(bytes.fromhex(regs[i:i + 8]), "little") for i in range(0, 16 * 8, 8)]
        return out
