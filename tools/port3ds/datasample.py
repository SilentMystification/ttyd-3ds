"""Data-address side of pc_sample.py (--data): what memory the sampled instructions touch.

ElfImage    reads instruction words from the ELF's loaded sections (no target reads).
DataMap     names an address: a [MEMMAP] region from the game's log, the ELF's data
            symbols, or an inferred thread stack. Big unsymbolled regions are split
            into 4 KiB pages, so a hot page shows up instead of a whole heap.
report()    top entries, regions, shared data, hot functions per entry, QTM candidates.
"""
import bisect
import collections
import math
import re
import struct
from pathlib import Path

import armdata
import elfsyms

PAGE = 4096
QTM_LO, QTM_HI = 0x1E800000, 0x1EA00000
MEMMAP_RE = re.compile(r"\[MEMMAP\] (\S+) 0x([0-9a-f]+) 0x([0-9a-f]+)")


class ElfImage:
    def __init__(self, path):
        d = Path(path).read_bytes()
        end = ">" if d[5] == 2 else "<"
        shoff = struct.unpack_from(end + "I", d, 0x20)[0]
        shentsize, shnum, shstrndx = struct.unpack_from(end + "HHH", d, 0x2E)
        self.secs = []  # (addr, end, bytes, name)
        shs = [struct.unpack_from(end + "IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
        stroff = shs[shstrndx][4]
        for name, typ, flags, addr, off, size, *_ in shs:
            if typ in (1, 8) and flags & 2 and size:  # PROGBITS or NOBITS, ALLOC
                n = d[stroff + name:d.index(b"\0", stroff + name)].decode()
                self.secs.append((addr, addr + size, d[off:off + size] if typ == 1 else None, n))
        self.secs.sort()
        self.starts = [s[0] for s in self.secs]

    def word(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            lo, hi, data, _ = self.secs[i]
            if data is not None and lo <= addr and addr + 4 <= hi:
                return struct.unpack_from("<I", data, addr - lo)[0]
        return None

    def section(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and self.secs[i][0] <= addr < self.secs[i][1]:
            return self.secs[i][3]
        return None


def read_memmap(path):
    """Latest [MEMMAP] line per name -> [(name, lo, hi)]."""
    regs = {}
    for line in Path(path).read_text(errors="replace").splitlines():
        m = MEMMAP_RE.search(line)
        if m:
            regs[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return [(n, lo, hi) for n, (lo, hi) in regs.items()]


class DataMap:
    def __init__(self, elf, image, memmap, stacks):
        """stacks: {tid_name: (lo, hi)} inferred thread stacks."""
        self.image = image
        objs = [s for s in elfsyms.load_symbols(elf).values() if s.kind == "object" and s.size]
        objs.sort(key=lambda s: s.addr)
        self.oaddrs = [s.addr for s in objs]
        self.objs = objs
        # smallest enclosing region first
        self.regions = sorted(memmap, key=lambda r: r[2] - r[1])
        self.stacks = stacks
        self.cache = {}

    def resolve(self, addr):
        """-> (entry key, size, region name)"""
        hit = self.cache.get(addr)
        if hit:
            return hit
        hit = self._resolve(addr)
        if len(self.cache) < 2_000_000:
            self.cache[addr] = hit
        return hit

    def _resolve(self, addr):
        for name, (lo, hi) in self.stacks.items():
            if lo <= addr < hi:
                return (f"stack:{name}", hi - lo, "stack")
        i = bisect.bisect_right(self.oaddrs, addr) - 1
        if i >= 0:
            s = self.objs[i]
            if s.addr <= addr < s.addr + s.size:
                return (s.name, s.size, "elf:" + (self.image.section(addr) or "?"))
        sec = self.image.section(addr)
        if QTM_LO <= addr < QTM_HI:
            base = addr & ~(PAGE - 1)
            return (f"qtm+0x{base - QTM_LO:x}", PAGE, "qtm")
        for name, lo, hi in self.regions:
            if name.startswith("vm_"):
                continue
            if lo <= addr < hi:
                if name.startswith("rel_"):
                    return (f"{name}+0x{(addr - lo) & ~0xFFF:x}", min(PAGE, hi - lo), name)
                base = lo + ((addr - lo) & ~(PAGE - 1))
                return (f"{name}+0x{base - lo:x}", PAGE, name)
        if sec:
            base = addr & ~(PAGE - 1)
            return (f"{sec}@0x{base:x}", PAGE, "elf:" + sec)
        for name, lo, hi in self.regions:
            if lo <= addr < hi:
                base = lo + ((addr - lo) & ~(PAGE - 1))
                return (f"{name}+0x{base - lo:x}", PAGE, name)
        return (f"unmapped@0x{addr & ~(PAGE - 1):x}", PAGE, "unknown")


def infer_stacks(samples, names):
    """Per thread: the SP range seen, widened a little -> {name: (lo, hi)}."""
    rng = {}
    for _, tid, _pc, _lr, sp, _tag in samples:
        lo, hi = rng.get(tid, (sp, sp))
        rng[tid] = (min(lo, sp), max(hi, sp))
    out = {}
    for tid, (lo, hi) in rng.items():
        out[names.get(tid, format(tid, "x"))] = ((lo - 0x2000) & ~3, (hi + 0x400 + 3) & ~3)
    return out


def core_of(name):
    n = name.lower()
    if "game" in n or "main" in n:
        return 0
    if "render" in n or "gpu" in n or "gsp" in n:
        return 2
    if "audio" in n or "dvd" in n or "disc" in n:
        return 1
    return None


def aggregate(accesses, dm, names, cores, funcname):
    """accesses: [(tid, pc, addr, size, wr)] -> {entry: info}"""
    ent = {}
    for tid, pc, addr, size, wr in accesses:
        key, esize, region = dm.resolve(addr)
        e = ent.get(key)
        if e is None:
            e = ent[key] = {"entry": key, "bytes": esize, "region": region, "n": 0, "r": 0, "w": 0,
                            "threads": collections.Counter(), "funcs": collections.Counter()}
        e["n"] += 1
        e["w" if wr else "r"] += 1
        e["threads"][names.get(tid, format(tid, "x"))] += 1
        e["funcs"][funcname(pc)] += 1
    for e in ent.values():
        e["cores"] = sorted({cores.get(t) for t in e["threads"] if cores.get(t) is not None})
        e["shared"] = len(e["threads"]) >= 2
        e["cross_core"] = len(e["cores"]) >= 2
    return ent


def pm(n, total):
    """Share and a rough 95% half-width in percentage points (binomial)."""
    if not total:
        return 0.0, 0.0
    p = n / total
    return 100 * p, 196 * math.sqrt(p * (1 - p) / total)


def build(ent, total, top, qtm_mb, min_shared):
    rows = sorted(ent.values(), key=lambda e: -e["n"])
    out = {"total_data_samples": total, "top": [], "regions": [], "shared": [], "qtm": []}
    for e in rows[:top]:
        p, h = pm(e["n"], total)
        out["top"].append({**e, "pct": p, "pm": h, "threads": dict(e["threads"]),
                           "funcs": e["funcs"].most_common(3)})
    reg = collections.defaultdict(lambda: {"n": 0, "r": 0, "w": 0, "threads": collections.Counter()})
    for e in ent.values():
        g = reg[e["region"]]
        g["n"] += e["n"]; g["r"] += e["r"]; g["w"] += e["w"]; g["threads"].update(e["threads"])
    for name, g in sorted(reg.items(), key=lambda kv: -kv[1]["n"])[:20]:
        p, h = pm(g["n"], total)
        out["regions"].append({"region": name, "n": g["n"], "pct": p, "pm": h, "r": g["r"], "w": g["w"],
                               "threads": dict(g["threads"])})
    for e in rows:
        if e["shared"] and e["n"] >= min_shared and sum(1 for c in e["threads"].values() if c >= 2) >= 2:
            out["shared"].append({**e, "threads": dict(e["threads"]), "funcs": e["funcs"].most_common(2)})
    out["shared"] = out["shared"][:40]
    cand = [e for e in rows if e["n"] >= 3 and e["region"] not in ("unknown",)]
    cand.sort(key=lambda e: -e["n"] / max(e["bytes"], 1))
    cum = 0
    for e in cand:
        cum += e["bytes"]
        out["qtm"].append({"entry": e["entry"], "n": e["n"], "bytes": e["bytes"], "density": e["n"] / e["bytes"] * 1024,
                           "shared": e["shared"], "cross_core": e["cross_core"], "cores": e["cores"],
                           "region": e["region"], "cum_bytes": cum,
                           "in_qtm": e["region"] == "qtm"})
        if cum > qtm_mb * 1048576 * 1.25:
            break
    # hot functions per hot entry
    out["hot_funcs"] = [{"entry": e["entry"], "funcs": e["funcs"].most_common(5)} for e in rows[:8]]
    return out


def format_text(res, title="", qtm_mb=1.6, brief=False):
    L = []
    t = res["total_data_samples"]
    L.append(f"== data accesses {title}: {t} samples (rough 95% half-width shown as +-pp; a count n has +-{1.96:.2f}*sqrt(n))")
    L.append("  rank share%  +-   n     r/w      bytes  threads                      entry  [region]")
    for i, e in enumerate(res["top"][:10 if brief else 25], 1):
        th = ",".join(f"{k}:{v}" for k, v in sorted(e["threads"].items(), key=lambda kv: -kv[1])[:3])
        L.append(f"  {i:4d} {e['pct']:6.2f} {e['pm']:4.2f} {e['n']:5d} {e['r']}/{e['w']:<6} {e['bytes']:8d}  {th:<28} "
                 f"{e['entry']}  [{e['region']}]")
    if brief:
        L.append("  QTM candidates (n/KiB, cum KiB): " + "; ".join(
            f"{e['entry']} {e['density']:.1f} {e['cum_bytes'] / 1024:.0f}" for e in res["qtm"][:6]))
        return "\n".join(L)
    L.append("  regions:")
    for r in res["regions"][:12]:
        L.append(f"    {r['pct']:6.2f}% {r['n']:6d}  r/w {r['r']}/{r['w']}  {r['region']}")
    L.append("  hot functions of the top entries:")
    for h in res["hot_funcs"]:
        L.append(f"    {h['entry']}: " + ", ".join(f"{f} {n}" for f, n in h["funcs"]))
    L.append("  shared data (two or more threads, each seen at least twice):")
    for e in res["shared"][:15]:
        L.append(f"    n={e['n']:5d} cores {e['cores']} {'CROSS-CORE ' if e['cross_core'] else ''}"
                 f"{','.join(f'{k}:{v}' for k, v in e['threads'].items())}  {e['entry']}")
    L.append(f"  QTM candidates (by samples per KiB; cumulative size vs {qtm_mb} MB):")
    L.append("    samples  bytes  n/KiB   cum KiB  flags  entry")
    for e in res["qtm"][:40]:
        fl = ("S" if e["shared"] else "-") + ("X" if e["cross_core"] else "-") + ("Q" if e["in_qtm"] else "-")
        L.append(f"    {e['n']:6d} {e['bytes']:7d} {e['density']:7.2f} {e['cum_bytes'] / 1024:8.0f}  {fl}   {e['entry']}")
    return "\n".join(L)
