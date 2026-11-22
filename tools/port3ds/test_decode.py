#!/usr/bin/env python3
"""Tests of the port's disc-format converters, on the host: docs/PORT_3DS_HOST_DECODE.md.

    tools/port3ds/test_decode.py [--disc iso/game.iso] [--papercut ../paper-cut] [--full] [--keep DIR]

Builds tools/port3ds/host_decode (the port's converters compiled for the host), runs it, and cross-checks it
against paper-cut's decoders (MIT, https://github.com/LLCoolT/paper-cut; read-only, never modified).
The checks and what each compares are listed in the document. Prints one line per check and a summary;
exit status 1 when a check fails, 0 when all pass (a missing paper-cut or disc image skips what needs it).
"""
import argparse
import collections
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

# Files paper-cut's C decoders reject that the port's converter accepts, understood: the retail game never loads them.
KNOWN_PAPERCUT_REJECTS = {
    "a/OFF_hei_01b": "pre-release model format with 0x68-byte shapes; hei_01 loads OFF_hei_01c. The port's converter "
                     "swaps it with the retail layout, so the result is wrong but never read",
}

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main_repo():
    """The checkout this worktree belongs to (the disc image and paper-cut live next to it)"""
    git = os.path.join(REPO, ".git")
    if os.path.isfile(git):
        gitdir = open(git).read().split(":", 1)[1].strip()
        common = os.path.normpath(os.path.join(REPO, gitdir, "..", ".."))
        if os.path.basename(common) == ".git":
            return os.path.dirname(common)
        # .git/worktrees/<name>
        return os.path.dirname(os.path.dirname(os.path.dirname(common)))
    return REPO


# ------------------------------------------------------------------ results ----

class Results:
    def __init__(self):
        self.rows = []  # (name, status, detail)

    def add(self, name, status, detail=""):
        self.rows.append((name, status, detail))
        print(f"{status:5} {name}" + (f": {detail}" if detail else ""), flush=True)

    def failed(self):
        return [r for r in self.rows if r[1] == "FAIL"]


class Check:
    """Collects mismatches of one check; reports the first few."""

    def __init__(self, res, name):
        self.res, self.name = res, name
        self.n = 0
        self.bad = []
        self.note = ""

    def ok(self, n=1):
        self.n += n

    def fail(self, what):
        self.bad.append(what)

    def done(self, extra=""):
        if self.bad:
            shown = "; ".join(self.bad[:4]) + (f"; ... {len(self.bad)} in all" if len(self.bad) > 4 else "")
            self.res.add(self.name, "FAIL", f"{len(self.bad)} mismatches of {self.n + len(self.bad)}: {shown}")
        else:
            self.res.add(self.name, "ok", f"{self.n} compared{extra}")


# --------------------------------------------------------------- the tool ----

def build_tool(args):
    exe = args.exe or os.path.join(REPO, "build", "host_decode", "host_decode")
    if not args.exe:
        bdir = os.path.join(REPO, "build", "host_decode")
        subprocess.run(["cmake", "-S", os.path.join(REPO, "tools/port3ds/host_decode"), "-B", bdir, "-G", "Ninja"],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["cmake", "--build", bdir], check=True, stdout=subprocess.DEVNULL)
    return exe


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def u32(b, a, e=">"):
    return struct.unpack_from(e + "I", b, a)[0]


# ------------------------------------------------------------------- TPL ----

def _tpl_walk(b, e, tpl2png, pixels=True):
    out = []
    magic, n, desc = struct.unpack_from(e + "III", b, 0)
    for i in range(n):
        th, ch = struct.unpack_from(e + "II", b, desc + 8 * i)
        entry = {}
        if th:
            h, w, fmt, off = struct.unpack_from(e + "HHII", b, th)
            rest = struct.unpack_from(e + "5I", b, th + 12)
            entry["tex"] = (h, w, fmt, off) + rest + (bytes(b[th + 32:th + 36]),)
        if ch:
            cnt, un, pad, pfmt, poff = struct.unpack_from(e + "HBBII", b, ch)
            entry["clut"] = (cnt, un, pad, pfmt, poff)
        out.append(entry)
    return out


def tpl_pixels(b, e, entry, tpl2png, ttyd_tpl):
    h, w, fmt, off = entry["tex"][:4]
    if fmt not in ttyd_tpl.BLOCKS:
        return None
    levels = 1
    size = ttyd_tpl.data_size(w, h, fmt)
    data = b[off:off + size]
    pal = None
    if "clut" in entry:
        cnt, _u, _p, pfmt, poff = entry["clut"]
        pal = tpl2png.palette(b[poff:poff + 2 * cnt], 0, cnt, pfmt)
    return tpl2png.decode(data, w, h, fmt, 0, pal)


# ------------------------------------------------------------------ anim ----

# The model header: counts at 0xE8 in this order (paper-cut ttyd_anim._C_*): shape, drawcall, pos, posidx, nrm,
# nrmidx, clr, clridx, texidx[8], texvtx, texanim, texdata, texbind, draw, vis, node, group, anim.
CNT = dict(shape=0, drawcall=1, pos=2, posidx=3, nrm=4, nrmidx=5, clr=6, clridx=7, texvtx=16, texanim=17,
           texdata=18, texbind=19, draw=20, vis=21, node=22, group=23, anim=24)
# table order of ttyd_anim.TABLES (the pointer words at 0x14C follow it)
PTR = dict(shapes=0, drawcalls=1, pos=2, posidx=3, nrm=4, nrmidx=5, clr=6, clridx=7, texvtx=16, texanim=17,
           texdata=18, texbind=19, draw=20, vis=21, node=22, group=23, anim=24)


def counts_of(counts, what):
    return counts[CNT[what]]


def anim_facts(b, e, pose=None):
    """The facts of a model read directly from the bytes in byte order e ('>' disc, '<' converted)."""
    f = {}
    counts = struct.unpack_from(e + "25I", b, 0xE8)
    ptrs = struct.unpack_from(e + "25I", b, 0x14C)
    f["counts"] = counts
    f["bbox"] = struct.unpack_from(e + "6f", b, 0xD0)
    f["names"] = (bytes(b[4:0x44]).split(b"\0")[0], bytes(b[0x44:0x84]).split(b"\0")[0])
    g = ptrs[PTR["group"]]
    f["groups"] = [(bytes(b[g + 0x58 * i:g + 0x58 * i + 0x40]).split(b"\0")[0],) + struct.unpack_from(e + "6i", b, g + 0x58 * i + 0x40)
                   for i in range(counts[CNT["group"]])]
    n = ptrs[PTR["node"]]
    f["node"] = struct.unpack_from(e + f"{counts[CNT['node']]}f", b, n) if counts[CNT["node"]] else ()
    f["vis"] = bytes(b[ptrs[PTR["vis"]]:ptrs[PTR["vis"]] + counts[CNT["vis"]]])
    for k in ("pos", "nrm"):
        c = counts[CNT[k]]
        f[k] = struct.unpack_from(e + f"{3 * c}f", b, ptrs[PTR[k]]) if c else ()
        c = counts[CNT[k + "idx"]]
        f[k + "idx"] = struct.unpack_from(e + f"{c}I", b, ptrs[PTR[k + "idx"]]) if c else ()
    c = counts[CNT["texvtx"]]
    f["texvtx"] = struct.unpack_from(e + f"{2 * c}f", b, ptrs[PTR["texvtx"]]) if c else ()
    anims = []
    a0, na = ptrs[24], counts[24]
    for j in range(na):
        rec = a0 + 0x40 * j
        name = bytes(b[rec:rec + 0x3C]).split(b"\0")[0]
        doff = u32(b, rec + 0x3C, e)
        if doff < a0 + 0x40 * na:
            anims.append((name, None))
            continue
        h = struct.unpack_from(e + "9I8I6f", b, doff)
        (_size, n_loop, n_key, n_pos, n_nrm, n_tex, n_vis, n_node, n_val) = h[:9]
        p_loop, p_key, p_pos, p_nrm, p_tex, p_vis, p_node, p_val = h[9:17]
        loops = [struct.unpack_from(e + "iff", b, doff + p_loop + 12 * i) for i in range(n_loop)]
        keys = [struct.unpack_from(e + "f10i", b, doff + p_key + 0x2C * k) for k in range(n_key)]
        vals = [struct.unpack_from(e + "fi", b, doff + p_val + 8 * i) for i in range(n_val)]
        anims.append((name, (h[:9], h[17:23], loops, keys, vals)))
    f["anims"] = anims
    return f


def anim_facts_pose(pose, b):
    """The same facts, from paper-cut's parse of the disc file (ttyd_anim.PoseFile: its tables are lists of raw
    big-endian records)"""
    import struct as st
    f = {}
    counts = st.unpack_from(">25I", b, 0xE8)
    f["counts"] = counts
    f["bbox"] = pose.bbox
    f["names"] = (pose.file_name.encode("latin-1"), pose.texture_name.encode("latin-1"))
    f["groups"] = [(g.name.encode("latin-1"), g.brother, g.child, g.shape, g.vis, g.node, g.ssc) for g in pose.groups()]
    f["node"] = tuple(pose.node_values())
    f["vis"] = bytes(v & 0xFF for v in pose.vis_values())
    for k, t in (("pos", "pos"), ("nrm", "nrm")):
        f[k] = tuple(x for r in pose.tables[t] for x in st.unpack(">3f", r))
        f[k + "idx"] = tuple(st.unpack(">I", r)[0] for r in pose.tables[t + "Idx"])
    f["texvtx"] = tuple(x for r in pose.tables["texVtx"] for x in st.unpack(">2f", r))
    anims = []
    for a in pose.anims:
        if getattr(a, "model_offset", None) is not None:
            anims.append((a.name.encode("latin-1"), None))
            continue
        anims.append((a.name.encode("latin-1"), a))
    f["anims"] = anims
    return f


# ------------------------------------------------------------------- map ----

class MapReader:
    """Walks a map file's joints, meshes and vertex arrays in byte order e; the same code reads a disc file ('>')
    and a converted one ('<'). Pointer words are the ones the relocation table lists, as the game's loader
    treats them."""

    def __init__(self, b, e):
        self.b, self.e = b, e
        self.size, self.dsize, self.nrel, self.nchunk, self.next = struct.unpack_from(e + "5I", b, 0)
        self.rel = set(0x20 + u32(b, 0x20 + self.dsize + 4 * i, e) for i in range(self.nrel))
        at = 0x20 + self.dsize + 4 * self.nrel
        strings = at + 8 * self.nchunk + 8 * self.next
        self.chunks = {}
        for i in range(self.nchunk):
            off, name = struct.unpack_from(e + "II", b, at + 8 * i)
            nm = bytes(b[strings + name:b.index(b"\0", strings + name)]).decode("latin-1")
            self.chunks[nm] = 0x20 + off
        self.seen = set()
        self.joints = []
        self.meshes = []
        self.arrays = []

    def w(self, off):
        return u32(self.b, off, self.e)

    def ptr(self, field):
        if field not in self.rel:
            return 0
        v = self.w(field)
        return 0x20 + v if v else 0

    def cstr(self, off):
        return bytes(self.b[off:self.b.index(b"\0", off)]).decode("latin-1") if off else ""

    def facts(self):
        e, b = self.e, self.b
        info = self.chunks["information"]
        ver = self.cstr(self.ptr(info))
        self.ver102 = ver == "ver1.02"
        self.walk_joint(self.ptr(info + 4))
        f = dict(header=(self.size, self.dsize, self.nrel, self.nchunk, self.next), chunks=sorted(self.chunks), ver=ver,
                 joints=self.joints, meshes=self.meshes, arrays=self.arrays)
        if "light_table" in self.chunks:
            f["lights"] = self.w(self.chunks["light_table"])
        if "animation_table" in self.chunks:
            at = self.chunks["animation_table"]
            n = self.w(at)
            f["anims"] = [struct.unpack_from(e + "8I", b, self.ptr(at + 4 + 4 * i)) if self.ptr(at + 4 + 4 * i) else None
                          for i in range(n)]
        if "texture_table" in self.chunks:
            f["textable"] = struct.unpack_from(e + "I", b, self.chunks["texture_table"])
        return f

    def walk_joint(self, o):
        while o and o not in self.seen:
            self.seen.add(o)
            e, b = self.e, self.b
            parts = struct.unpack_from(e + "i", b, o + 0x5C)[0]
            self.joints.append((self.cstr(self.ptr(o)), self.cstr(self.ptr(o + 4)), struct.unpack_from(e + "6f", b, o + 0x3C),
                                parts, struct.unpack_from(e + "9f", b, o + 0x18)))
            for p in range(parts):
                self.walk_mesh(self.ptr(o + 0x60 + 8 * p + 4))
            self.walk_joint(self.ptr(o + 0xC))
            o = self.ptr(o + 0x10)

    def walk_mesh(self, o):
        if not o or o in self.seen:
            return
        self.seen.add(o)
        e, b = self.e, self.b
        packed = b[o + 3]
        n = struct.unpack_from(e + "i", b, o + 4)[0]
        vcd = self.ptr(o + 0xC)
        info = [packed, n]
        if packed:
            # {offset, size}: the list bytes stay big-endian; the size is converted
            info += [struct.unpack_from(e + "I", b, o + 0x10 + 8 * i + 4)[0] for i in range(n)]
        else:
            for i in range(n):
                dl = self.ptr(o + 0x10 + 4 * i)
                info.append(struct.unpack_from(e + "i", b, dl)[0] if dl else None)
        self.meshes.append(tuple(info))
        if vcd and vcd not in self.seen:
            self.seen.add(vcd)
            self.vcd(vcd)

    def vcd(self, v):
        e, b = self.e, self.b
        s16 = self.ver102
        pos, nrm = self.ptr(v), self.ptr(v + 4)
        rec = ["pos", "nrm", "clr", "tex"]
        if pos:
            n = u32(b, pos, e)
            if n and pos not in self.seen:
                self.seen.add(pos)
                if s16:
                    vals = struct.unpack_from(e + f"{3 * n}h", b, pos + 4)
                else:
                    vals = struct.unpack_from(e + f"{3 * n}f", b, pos + 4)
                self.arrays.append(("pos", n, tuple(min(vals[k::3]) for k in range(3)), tuple(max(vals[k::3]) for k in range(3))))
        if nrm:
            self.arrays.append(("nrm", u32(b, nrm, e)))
        for i in range(2):
            c = self.ptr(v + 0xC + 4 * i)
            if c:
                self.arrays.append(("clr", u32(b, c, e)))
        for i in range(8):
            t = self.ptr(v + 0x18 + 4 * i)
            if t:
                n = u32(b, t, e)
                if s16:
                    vals = struct.unpack_from(e + f"{2 * n}h", b, t + 4)
                else:
                    vals = struct.unpack_from(e + f"{2 * n}f", b, t + 4)
                self.arrays.append(("tex", n, tuple(min(vals[k::2]) for k in range(2)) if n else (), tuple(max(vals[k::2]) for k in range(2)) if n else ()))


# ----------------------------------------------------------------- main ----

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc", help="the US disc image (default $TTYD_DISC, iso/game.iso)")
    ap.add_argument("--papercut", help="paper-cut checkout (default $PAPERCUT, ../paper-cut)")
    ap.add_argument("--exe", help="a built host_decode (default: build it)")
    ap.add_argument("--full", action="store_true", help="compare every texture's pixels (default: a sample)")
    ap.add_argument("--keep", metavar="DIR", help="keep the converted files and builds in DIR")
    args = ap.parse_args()
    res = Results()
    t_start = time.time()

    here = main_repo()
    disc = args.disc or os.environ.get("TTYD_DISC") or next(
        (p for p in (os.path.join(REPO, "iso/game.iso"), os.path.join(here, "iso/game.iso")) if os.path.exists(p)), None)
    pc = args.papercut or os.environ.get("PAPERCUT") or next(
        (p for p in (os.path.join(REPO, "..", "paper-cut"), os.path.join(here, "..", "paper-cut")) if os.path.isdir(p)), None)
    if pc:
        pc = os.path.abspath(pc)
    exe = build_tool(args)

    # 1. swap tables: applied twice they are the identity
    r = run([exe, "selftest"])
    res.add("swap tables applied twice are the identity (random data, every struct)",
            "ok" if r.returncode == 0 else "FAIL", r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip())

    if not disc or not os.path.exists(disc):
        res.add("disc image", "SKIP", "no disc image (--disc, $TTYD_DISC or iso/game.iso)")
        return finish(res, t_start)
    os.environ["TTYD_DISC"] = disc
    scratch = args.keep or tempfile.mkdtemp(prefix="host_decode_test_")
    os.makedirs(scratch, exist_ok=True)
    try:
        return run_disc_checks(args, res, exe, disc, pc, scratch, t_start)
    finally:
        if not args.keep:
            shutil.rmtree(scratch, ignore_errors=True)


def finish(res, t_start):
    n = len(res.rows)
    bad = res.failed()
    skipped = [r for r in res.rows if r[1] == "SKIP"]
    print(f"\n{n - len(bad) - len(skipped)} passed, {len(bad)} failed, {len(skipped)} skipped, "
          f"{time.time() - t_start:.1f} s")
    return 1 if bad else 0


def run_disc_checks(args, res, exe, disc, pc, scratch, t_start):
    sys.path.insert(0, os.path.join(REPO, "tools", "port3ds"))
    have_pc = bool(pc) and os.path.isdir(os.path.join(pc, "tools", "port"))
    if have_pc:
        sys.path.insert(0, os.path.join(pc, "tools", "port"))
        import gcdisc
        import tpl2png
        import ttyd_tpl
        import ttyd_anim
        d = gcdisc.Disc(disc)
    else:
        res.add("paper-cut", "SKIP", "no paper-cut checkout (--papercut, $PAPERCUT or ../paper-cut): the cross-checks are skipped")
        d = None

    # 2. the FST listing against paper-cut's gcdisc.py
    r = run([exe, "list", "-l"])
    listing = {}
    for line in r.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0] != "-":
            path = parts[2].split("  [")[0]
            listing[path] = (int(parts[0], 16), int(parts[1]))
    if d:
        c = Check(res, "FST listing equals paper-cut's gcdisc.py (path, offset, size)")
        for name, (off, size) in d.files.items():
            got = listing.get(name)
            if got != (off, size):
                c.fail(f"{name}: {got} vs {(off, size)}")
            else:
                c.ok()
        for name in listing:
            if name not in d.files:
                c.fail(f"{name} only in host_decode")
        c.done()
    else:
        res.add("FST listing", "ok", f"{len(listing)} files (not compared)")

    # 3. every file decodes
    tsv = os.path.join(scratch, "all.tsv")
    outdir = os.path.join(scratch, "out")
    t0 = time.time()
    r = run([exe, "all", "--tsv", tsv, "--out", outdir])
    t_all = time.time() - t0
    table = [l for l in r.stdout.splitlines() if l and not l.startswith("FAIL")]
    fails = [l for l in r.stdout.splitlines() if l.startswith("FAIL")]
    res.add("host_decode all: every file the port converts converts, and converting again changes nothing",
            "ok" if r.returncode == 0 else "FAIL", f"{len(fails)} failures, {t_all:.1f} s")
    for l in fails:
        print("      " + l)
    print("\n".join("      " + l for l in table))

    # the converter must have changed every file it is given (no silent early return)
    unchanged = 0
    for l in table:
        p = l.split()
        if p and p[0] in ("tpl", "tpl-nosize", "anim", "map", "camroad", "font", "icon.bin") or (p and p[0].startswith("musyx")):
            if len(p) > 3 and p[3].isdigit():
                unchanged += int(p[3])
    res.add("every converted file differs from the disc file (nothing returned early)", "ok" if unchanged == 0 else "FAIL",
            f"{unchanged} unchanged")

    # 4. data compiled into the decomp, read from the image
    r = run([exe, "discdata"])
    res.add("discdata: port_disc_data_fill reads the memory card art and the texture arrays", "ok" if r.returncode == 0 else "FAIL",
            r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stdout + r.stderr)
    if r.returncode == 0 and d:
        check_discdata(res, r.stdout, d, disc)

    if not d:
        return finish(res, t_start)

    # per-file lists from the tsv
    files = collections.defaultdict(list)
    for line in open(tsv):
        f = line.rstrip("\n").split("\t")
        if f[2] == "ok":
            files[f[0]].append(f[1])

    # 5. paper-cut cross-checks
    check_tpl(res, files["tpl"], d, outdir, tpl2png, ttyd_tpl, args.full)
    check_anim(res, files["anim"], d, outdir, ttyd_anim)
    check_map(res, files["map"], d, outdir)
    check_permutation(res, files, d, outdir)
    check_musyx(res, files, d, outdir, pc, scratch)
    check_papercut_sweep(res, d, pc, scratch, tsv)
    return finish(res, t_start)


# ------------------------------------------------------------- the checks ----

def check_discdata(res, text, d, disc):
    """Reads the same arrays independently: main.dol sections by address, REL sections by offset"""
    sys.path.insert(0, os.path.join(REPO, "tools", "port3ds"))
    import gen_disc_data as g
    got = {l.split()[0]: (int(l.split()[1]), l.split()[2]) for l in text.splitlines() if len(l.split()) == 3}
    boot = open(disc, "rb").read(0x440)
    dol = u32(boot, 0x420)
    cfg = os.path.join(REPO, "config", "G8ME01")
    dol_syms = g.symbols(g.Path(cfg) / "symbols.txt")
    f = open(disc, "rb")

    def read(off, n):
        f.seek(off)
        return f.read(n)

    def fnv(b):
        h = 2166136261
        for x in b:
            h = ((h ^ x) * 16777619) & 0xFFFFFFFF
        return f"{h:08x}"

    # the main.dol sections
    hdr = read(dol, 0x100)
    secs = [(u32(hdr, i * 4), u32(hdr, 0x48 + i * 4), u32(hdr, 0x90 + i * 4)) for i in range(18)]
    c = Check(res, "discdata arrays equal an independent read of main.dol and the RELs")
    for key, (where, sym) in g.ENTRIES.items():
        if where == "dol":
            _, addr, size = dol_syms[sym]
            data = None
            for off, a, sz in secs:
                if sz and a <= addr and addr + size <= a + sz:
                    data = read(dol + off + addr - a, size)
            exp = (size, fnv(data)) if data is not None else None
        else:
            syms = g.symbols(g.Path(cfg) / where / "symbols.txt")
            section, off, size = syms[sym]
            idx = g.rel_sections(g.Path(cfg) / where / "splits.txt")[section]
            roff, rsize = d.files[f"rel/{where}.rel"]
            rel = read(roff, rsize)
            nsec, secinfo = u32(rel, 0xC), u32(rel, 0x10)
            soff = u32(rel, secinfo + idx * 8) & ~1
            exp = (size, fnv(rel[soff + off:soff + off + size]))
        if got.get(key) != exp:
            c.fail(f"{key}: {got.get(key)} vs {exp}")
        else:
            c.ok()
    c.done()


def check_tpl(res, paths, d, outdir, tpl2png, ttyd_tpl, full):
    c = Check(res, "TPL: texture headers (size, format, data and palette offsets, filter fields) equal paper-cut's parse")
    px = Check(res, "TPL: decoded pixels equal paper-cut's tpl2png decode (sample; --full: all)")
    step = 1 if full else 25
    for i, p in enumerate(paths):
        orig = d.read(p)
        conv = open(os.path.join(outdir, "tpl", p), "rb").read()
        try:
            t = ttyd_tpl.TplFile.parse(orig)
        except Exception as ex:
            c.fail(f"{p}: paper-cut cannot parse it ({ex})")
            continue
        facts = _tpl_walk(conv, "<", tpl2png)
        orig_facts = _tpl_walk(orig, ">", tpl2png)
        if len(facts) != len(t.images) or facts != orig_facts:
            c.fail(f"{p}: header graph differs")
            continue
        c.ok()
        if i % step == 0:
            for k, im in enumerate(t.images):
                if "tex" not in facts[k] or im.format not in ttyd_tpl.BLOCKS or im.width * im.height > 256 * 256:
                    continue
                try:
                    a = im.pixels()
                except Exception:
                    continue
                b = tpl_pixels(conv, "<", facts[k], tpl2png, ttyd_tpl)
                rows = [b[y * im.width:(y + 1) * im.width] for y in range(im.height)]
                if rows != a:
                    px.fail(f"{p}[{k}]")
                else:
                    px.ok()
    c.done()
    px.done()


def check_anim(res, paths, d, outdir, ttyd_anim):
    c = Check(res, "anim: model header, groups, node/vertex floats, index tables, animation names, loop info, "
                   "keyframes and value keys equal paper-cut's ttyd_anim parse")
    refused = []
    key_total = 0
    for p in paths:
        orig = d.read(p)
        conv = open(os.path.join(outdir, "anim", p), "rb").read()
        try:
            pose = ttyd_anim.PoseFile.parse(orig)
        except Exception as ex:
            refused.append(p)
            continue
        want = anim_facts_pose(pose, orig)
        got = anim_facts(conv, "<")
        for k in ("counts", "bbox", "names", "groups", "node", "vis", "pos", "nrm", "posidx", "nrmidx", "texvtx"):
            if k == "bbox":
                same = all(a == b or (a != a and b != b) for a, b in zip(want[k], got[k]))
            elif k in ("node", "pos", "nrm", "texvtx"):
                same = len(want[k]) == len(got[k]) and all(a == b or (a != a and b != b) for a, b in zip(want[k], got[k]))
            else:
                same = want[k] == got[k]
            if not same:
                c.fail(f"{p}: {k}")
                break
        else:
            if len(want["anims"]) != len(got["anims"]):
                c.fail(f"{p}: animation count")
                continue
            ok = True
            for (wn, wa), (gn, ga) in zip(want["anims"], got["anims"]):
                if wn != gn or (wa is None) != (ga is None):
                    ok = False
                    c.fail(f"{p}: animation {wn}")
                    break
                if wa is None:
                    continue
                if len(wa.keys) != len(ga[3]) or len(wa.loop_info) != len(ga[2]) or len(wa.value_keys) != len(ga[4]):
                    ok = False
                    c.fail(f"{p}: {wn} counts")
                    break
                key_total += len(ga[3])
                if [tuple(x) for x in wa.loop_info] != [tuple(x) for x in ga[2]] or [tuple(x) for x in wa.value_keys] != [tuple(x) for x in ga[4]]:
                    ok = False
                    c.fail(f"{p}: {wn} loop info or value keys")
                    break
                # an old-format AnimData (a 0x44-byte header, two map objects) has no bounding box
                old = getattr(wa, "header_size", None) == ttyd_anim.ANIM_HEADER_OLD
                if [k.time for k in wa.keys] != [k[0] for k in ga[3]] or (not old and tuple(wa.bbox) != tuple(ga[1])):
                    ok = False
                    c.fail(f"{p}: {wn} key times or bounds")
                    break
            if ok:
                c.ok()
    c.done(f" ({key_total} keyframes), paper-cut's parser refuses {len(refused)}"
           + (f" ({', '.join(refused[:3])}{', ...' if len(refused) > 3 else ''})" if refused else ""))


def check_map(res, paths, d, outdir):
    c = Check(res, "map: joints (names, bounds), meshes, vertex array counts and bounds, lights, animation table "
                   "equal a big-endian walk of the disc file (layout after paper-cut's mapdump.py)")
    nverts = 0
    for p in paths:
        orig = d.read(p)
        conv = open(os.path.join(outdir, "map", p), "rb").read()
        try:
            a = MapReader(orig, ">").facts()
            b = MapReader(conv, "<").facts()
        except Exception as ex:
            c.fail(f"{p}: {type(ex).__name__} {ex}")
            continue
        for k in a:
            if k == "joints":
                same = len(a[k]) == len(b[k]) and all(x[0] == y[0] and x[1] == y[1] and x[3] == y[3] and x[2] == y[2] and x[4] == y[4]
                                                       for x, y in zip(a[k], b[k]))
            else:
                same = a[k] == b[k]
            if not same:
                c.fail(f"{p}: {k}")
                break
        else:
            nverts += sum(x[1] for x in a["arrays"] if x[0] == "pos")
            c.ok()
    c.done(f" ({nverts} positions)")


def check_permutation(res, files, d, outdir):
    """A converter only reverses bytes inside 2- and 4-byte units: the bytes of the file are a permutation of
    the disc file's (camera roads also write an 8-byte marker)."""
    try:
        import numpy as np
    except ImportError:
        res.add("converted bytes are a permutation of the disc bytes", "SKIP", "no numpy")
        return
    c = Check(res, "converted file bytes are a permutation of the disc file's (nothing but byte reversal)")
    for fmt in ("tpl", "anim", "map", "camroad", "font", "icon.bin", "musyx-proj", "musyx-pool", "musyx-sdir", "musyx-slib"):
        for p in files.get(fmt, []):
            orig = d.read(p)
            fp = os.path.join(outdir, fmt, p)
            conv = open(fp, "rb").read()
            if fmt.startswith("musyx") or fmt == "font":
                conv = conv[:len(orig)]  # the game's read is rounded up to 32 bytes
            ho = np.bincount(np.frombuffer(orig, np.uint8), minlength=256)
            hc = np.bincount(np.frombuffer(conv, np.uint8), minlength=256)
            if fmt == "camroad":
                ho = ho - np.bincount(np.frombuffer(orig[:8], np.uint8), minlength=256) + np.bincount(np.frombuffer(conv[:8], np.uint8), minlength=256)
            if (ho != hc).any():
                c.fail(f"{fmt} {p}")
            else:
                c.ok()
    c.done()


def check_musyx(res, files, d, outdir, pc, scratch):
    """paper-cut's MusyX converters (port/src/common/musyx_swap.c, compiled as is) on the same files"""
    src = os.path.join(pc, "port", "src", "common", "musyx_swap.c")
    exe = os.path.join(scratch, "xcheck_musyx")
    if not os.path.exists(src):
        res.add("MusyX: paper-cut's converters", "SKIP", "no musyx_swap.c in paper-cut")
        return
    flags = ["-std=gnu99", "-O1", "-w", "-include", os.path.join(REPO, "tools/port3ds/host_decode/host_prefix.h"),
             "-DMUSY_VERSION_MAJOR=2", "-DMUSY_VERSION_MINOR=0", "-DMUSY_VERSION_PATCH=3", "-DMUSY_TARGET=0",
             f"-I{pc}/port/include", f"-I{pc}/include", f"-I{pc}/libs/musyx/include", f"-I{pc}/libs/dolsdk2004/include",
             f"-I{REPO}/tools/port3ds/host_decode"]
    r = run(["gcc"] + flags + ["-o", exe, os.path.join(REPO, "tools/port3ds/host_decode/xcheck_musyx.c"), src])
    if r.returncode != 0:
        res.add("MusyX: build paper-cut's converters", "FAIL", r.stderr.strip()[:300])
        return
    c = Check(res, "MusyX: .proj, .pool, .slib and .stm headers are byte-identical to paper-cut's converters")
    tmp = os.path.join(scratch, "pc_musyx_out")
    for kind, fmt in (("proj", "musyx-proj"), ("pool", "musyx-pool"), ("slib", "musyx-slib"), ("stm", "musyx-stm")):
        for p in files.get(fmt, []):
            infile = os.path.join(scratch, "pc_in")
            open(infile, "wb").write(d.read(p) if kind != "stm" else d.read(p)[:0x100])
            r = run([exe, kind, infile, tmp])
            if r.returncode != 0:
                c.fail(f"{p}: xcheck exit {r.returncode}")
                continue
            theirs = open(tmp, "rb").read()
            ours = open(os.path.join(outdir, fmt, p), "rb").read()
            if kind == "stm":
                ours = ours[:0x100]
            if ours[:len(theirs)] != theirs:
                c.fail(p)
            else:
                c.ok()
    c.done()
    c = Check(res, "MusyX: .sdir sample headers (id, refcount, offset, address, info, length, loop, ADPCM offset) and ADPCM records "
                   "equal paper-cut's rebuilt directory")
    for p in files.get("musyx-sdir", []):
        orig = d.read(p)
        infile = os.path.join(scratch, "pc_in")
        open(infile, "wb").write(orig)
        r = run([exe, "sdir", infile, tmp])
        theirs = [tuple(int(x) for x in l.split()) for l in r.stdout.splitlines()]
        ours = open(os.path.join(outdir, "musyx-sdir", p), "rb").read()
        mine = []
        i = 0
        while struct.unpack_from("<H", ours, i * 32)[0] != 0xFFFF:
            mine.append(struct.unpack_from("<HHIIIIIII", ours, i * 32))
            i += 1
        if mine != theirs or len(mine) == 0:
            c.fail(f"{p}: {len(mine)} entries vs {len(theirs)}")
            continue
        # ADPCM records and everything after the entry table: their copy of the file is converted only there
        table = (len(mine) + 1) * 32
        copy = open(tmp, "rb").read()
        if ours[table:len(copy)] != copy[table:]:
            c.fail(f"{p}: data after the table differs")
            continue
        c.ok(len(mine))
    c.done(" entries")


def check_papercut_sweep(res, d, pc, scratch, tsv):
    """paper-cut's disc_sweep (its C decoders over the disc's files) accepts the same files"""
    pcs = os.path.join(pc, "tests", "port", "disc_sweep.c")
    if not os.path.exists(pcs):
        res.add("paper-cut C decoders", "SKIP", "no tests/port/disc_sweep.c")
        return
    O = os.path.join(scratch, "pc_sweep")
    os.makedirs(O, exist_ok=True)
    S = os.path.join(pc, "port/src/common")
    flags = ["-DBUILD_VERSION=1", "-DNDEBUG=1", "-DNONMATCHING=1", "-DPORT_BEDECODE_STANDALONE=1", '-DPORT_GAME_ID="G8ME01"',
             "-DPORT_MODS=1", "-DSDK_REVISION=1", "-DTARGET_PC=1", "-DTTYD_BACKEND_AURORA=1", "-DVERSION_G8ME01",
             "-DTTYD_PORT_SOURCE=1", f"-I{pc}/port/include", f"-I{pc}/include", f"-I{pc}/libs/musyx/include",
             f"-I{pc}/port/include/aurora_overlay", f"-I{pc}/extern/aurora/include", f"-I{pc}/port/include/no_disc_assets",
             "-O2", "-std=gnu11", "-ffp-contract=off", "-include", f"{pc}/port/include/port_prefix.h", "-iquote",
             f"{pc}/include/sdk", "-funsigned-char", "-fno-strict-aliasing", "-fwrapv", "-fcommon", "-fgnu89-inline", "-w"]
    srcs = [pcs] + [os.path.join(S, n) for n in (
        "bedecode.c bedecode_gen_anim.c bedecode_gen_map.c bedecode_gen_tpl.c bedecode_gen_camroad.c bedecode_gen_font.c "
        "anim_pc.c map_pc.c tpl_pc.c camroad_pc.c font_pc.c gx_arrays_pc.c").split()]
    exe = os.path.join(O, "disc_sweep")
    r = run(["gcc"] + flags + ["-o", exe] + srcs + ["-lm"])
    if r.returncode != 0:
        res.add("paper-cut C decoders: build disc_sweep", "SKIP", r.stderr.strip()[:200])
        return
    # the files it reads, laid out as on the disc
    lines = []
    for name in sorted(d.files):
        parts = name.split("/")
        kind = None
        n = parts[-1]
        if parts[0] == "a" and len(parts) == 2 and "." not in n:
            kind = "tpl" if n.endswith("-") else "anim"
        elif parts[0] == "m" and len(parts) == 3 and n in ("d", "c", "t"):
            kind = {"d": "map", "c": "camroad", "t": "tpl"}[n]
        elif n.endswith(".tpl"):
            kind = "tpl"
        elif n.endswith(".bfn"):
            kind = "font"
        if kind:
            dst = os.path.join(O, "files", name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(d.read(name))
            lines.append(f"{kind} {dst}\n")
    r = subprocess.run([exe], input="".join(lines), capture_output=True, text=True)
    theirs_fail = {l.split()[2].rstrip(":"): l for l in r.stdout.splitlines() if l.startswith("FAIL ")}
    summary = [l for l in r.stdout.splitlines() if l.startswith("disc_sweep:")]
    ours_fail = set()
    for line in open(tsv):
        f = line.rstrip("\n").split("\t")
        if f[2] == "FAIL":
            ours_fail.add(f[1])
    theirs = {os.path.relpath(p, os.path.join(O, "files")): l for p, l in theirs_fail.items()}
    both = [p for p in theirs if p in ours_fail]
    only_theirs = [p for p in theirs if p not in ours_fail and p not in KNOWN_PAPERCUT_REJECTS]
    known = [p for p in theirs if p not in ours_fail and p in KNOWN_PAPERCUT_REJECTS]
    only_ours = [p for p in ours_fail if p not in theirs]
    detail = (summary[0] if summary else f"exit {r.returncode}") + f"; host_decode fails {len(ours_fail)}"
    if known:
        detail += "; known difference: " + ", ".join(f"{p} ({KNOWN_PAPERCUT_REJECTS[p]})" for p in known)
    if only_theirs or only_ours:
        res.add("paper-cut's C decoders (disc_sweep) and host_decode accept the same files", "FAIL",
                detail + "; only paper-cut rejects: " + ", ".join(only_theirs[:6]) +
                ("; only host_decode rejects: " + ", ".join(only_ours[:6]) if only_ours else ""))
    else:
        res.add("paper-cut's C decoders (disc_sweep) and host_decode accept the same files", "ok", detail)
    for p in only_theirs + known:
        print("      paper-cut rejects, host_decode accepts: " + theirs_fail[os.path.join(O, "files", p)][5:])


if __name__ == "__main__":
    sys.exit(main())
