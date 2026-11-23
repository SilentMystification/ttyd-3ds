#!/usr/bin/env python3
"""Systems tests in Azahar: saves in every slot, copy and delete, soft reset, game over, the pause menu, mail and
the journal (backlog lane B item 6). The list is in docs/PORT_3DS_TESTING.md, "Systems tests".

    tools/port3ds/systems_test.py                    # every scenario, in order
    tools/port3ds/systems_test.py save_new reset     # some of them (a scenario's card comes from the one before)
    tools/port3ds/systems_test.py --list
    tools/port3ds/systems_test.py adhoc --switches "..." --padscript "..." [--card FIXTURE] [--frames N]

Each scenario is one run of tools/port3ds/run_azahar.py (headless, under its Azahar slot lock) with its own profile
(build/systems-profile/: the SD card's card/ folder is emptied or filled from a fixture first), a padscript under
`determ` (so a run repeats frame for frame), and checks on the game log (the `sys:` lines of the syslog switch,
`card:` lines, gwdump records), the GCI file left on the SD card (test_gci.py's checks) and screenshots.

A scenario that leaves a card behind saves it as a fixture (build/systems-test/cards/<name>.gci); later scenarios
start from it. Results: build/systems-test/<scenario>/ (game log, shots as PNG, card) and results.json.
Exit status 0 when every scenario run passes.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "port3ds"))
import test_gci  # noqa: E402

PROFILE = ROOT / "build" / "systems-profile"
SDMC = PROFILE / "data" / "azahar-emu" / "sdmc"
GAME_DIR = SDMC / "3ds" / "ttyd"
OUT = ROOT / "build" / "systems-test"
CARDS = OUT / "cards"
SAVE_GCI = "01-G8ME-mariost_save_file.gci"

# Boot to the file select under determ: fastboot presses A through the logos and stops at the title; the title's
# press-start and its menu then take A (frames counted from the first VIWaitForRetrace).
TO_FILE_SELECT = "wait*700,a*3,wait*120,a*3,wait*200"


def pad(events, end=None):
    """A padscript from absolute game frames: events are (frame, keys, frames held), in order."""
    out, at = [], 1
    for frame, keys, hold in events:
        if frame < at:
            raise ValueError(f"pad event at frame {frame} overlaps the one before (next free frame {at})")
        if frame > at:
            out.append(f"wait*{frame - at}")
        out.append(f"{keys}*{hold}")
        at = frame + hold
    if end is not None and end > at:
        out.append(f"wait*{end - at}")
    return ",".join(out)


def find_iso():
    for p in (ROOT / "iso" / "game.iso", ROOT.parents[2] / "iso" / "game.iso" if len(ROOT.parents) > 2 else None):
        if p is not None and p.is_file():
            return p
    common = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=ROOT, capture_output=True, text=True)
    if common.returncode == 0:
        p = (ROOT / common.stdout.strip()).resolve().parent / "iso" / "game.iso"
        if p.is_file():
            return p
    return None


def prepare_profile(card):
    """The SD card for one run: the disc linked in, card/ emptied or set to a fixture, old shots removed."""
    GAME_DIR.mkdir(parents=True, exist_ok=True)
    iso = GAME_DIR / "game.iso"
    if not iso.exists():
        src = find_iso()
        if src is None:
            sys.exit("systems_test: no iso/game.iso (see docs/PORT_3DS_TESTING.md, one-time setup)")
        iso.hardlink_to(src)
    shutil.rmtree(GAME_DIR / "card", ignore_errors=True)
    (GAME_DIR / "card").mkdir()
    if card is not None:
        src = CARDS / f"{card}.gci"
        if not src.is_file():
            raise FileNotFoundError(f"fixture {src.name} is missing: run the scenario that makes it first")
        shutil.copy(src, GAME_DIR / "card" / SAVE_GCI)
    shutil.rmtree(GAME_DIR / "shots", ignore_errors=True)
    for f in ("log.txt", "log.old", "crash.txt", "options.txt"):
        (GAME_DIR / f).unlink(missing_ok=True)


def run_game(build, switches, padscript, until, timeout, last, expect_exit=False):
    sw = f"determ syslog gwdump=30 {switches}"
    if padscript:
        sw += f" padscript={padscript}"
    cmd = [sys.executable, str(ROOT / "tools/port3ds/run_azahar.py"), "--build", str(build), "--profile", str(PROFILE),
           "--last", str(last), "--switches", sw, "--log", "3ds/ttyd/log.txt", "--timeout", str(timeout),
           "--stall", "90"]
    if until:
        cmd += ["--until", until]
    t0 = time.monotonic()
    child = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        out, _ = child.communicate()
    except BaseException:
        child.terminate()  # run_azahar.py stops the Azahar it started
        try:
            child.wait(timeout=30)
        except subprocess.TimeoutExpired:
            child.kill()
        raise
    log_path = last / "game_log.txt"
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    return child.returncode, out, log, time.monotonic() - t0


def collect_shots(dest):
    shots = GAME_DIR / "shots"
    got = []
    if shots.is_dir():
        dest.mkdir(parents=True, exist_ok=True)
        for ppm in sorted(shots.glob("*.ppm")):
            if "." in ppm.stem:
                continue
            try:
                from PIL import Image
                im = Image.open(ppm)
                im.load()
                png = dest / (ppm.stem + ".png")
                im.save(png)
                got.append((png, im))
            except OSError:
                continue  # the last shot, cut off when Azahar was stopped
            except ImportError:
                shutil.copy(ppm, dest / ppm.name)
                got.append((dest / ppm.name, None))
    return got


# ---- log parsing ----

def sys_lines(log):
    return [l.split("sys: ", 1)[1] for l in log.splitlines() if "sys: " in l]


def seq_path(log):
    """The sequence changes: [(from, to, frame, map)]"""
    out = []
    for l in sys_lines(log):
        m = re.match(r"seq (-?\d+) -> (-?\d+) at frame (\d+) map (\S*)", l)
        if m:
            out.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)))
    return out


def gwdumps(log):
    """gwdump records joined by frame: {frame: {field: value}}"""
    recs = {}
    for l in log.splitlines():
        m = re.search(r"gwdump f=(\d+) (.*)", l)
        if not m:
            continue
        r = recs.setdefault(int(m.group(1)), {})
        for kv in m.group(2).split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                r[k] = v
    return recs


def last_gw(log, pred=lambda r: True):
    recs = gwdumps(log)
    for f in sorted(recs, reverse=True):
        if pred(recs[f]):
            return f, recs[f]
    return None, None


def fbits(h):
    import struct
    return struct.unpack(">f", bytes.fromhex(h))[0]


def error_lines(log):
    bad = []
    for l in log.splitlines():
        if "[CRASH]" in l or re.search(r"\b(ERROR|fatal)\b", l) or re.search(r"^#\d+ \[E\]", l):
            bad.append(l)
    return bad


def card_state(path):
    """test_gci's view of a GCI: (gci object, slot list [(main ok, backup ok, flags)])"""
    g = test_gci.parse(path.read_bytes())
    test_gci.check_entry(g)
    test_gci.check_save(g)
    return g


def slot_used(g, i):
    """A slot holds a save: a valid copy and the empty flag (CARD_SAVE_FLAG_EMPTY, 1) clear."""
    a, b, flags = g.slots[i]
    return (a or b) and not (flags & 1)


# ---- scenarios ----

class Ctx:
    def __init__(self, name, log, out, rc, shots, card):
        self.name, self.log, self.out, self.rc, self.shots, self.card = name, log, out, rc, shots, card
        self.results = []

    def check(self, ok, text):
        self.results.append((bool(ok), text))
        return ok


SCENARIOS = []


def scenario(name, card_in=None, card_out=None, timeout=600, until=None, switches="", pad="", doc=""):
    def deco(fn):
        SCENARIOS.append(dict(name=name, card_in=card_in, card_out=card_out, timeout=timeout, until=until,
                              switches=switches, pad=pad, check=fn, doc=doc))
        return fn
    return deco


def until_frame(n):
    """run_azahar --until for game frame n (a gwdump record: every 30 frames)."""
    assert n % 30 == 0, n
    return rf"gwdump f={n} "


def card_writes(log):
    """card: write lines as (offset, length)"""
    return [(int(m.group(1), 16), int(m.group(2), 16))
            for m in re.finditer(r"card: write\s+offset 0x([0-9a-f]+) length 0x([0-9a-f]+) result 0", log)]


SLOT_SIZE = 0x4000


def slot_writes(log):
    """The save slots written (main copy at 0x2000 + slot * 0x4000)"""
    return [(o - 0x2000) // SLOT_SIZE for o, n in card_writes(log) if 0x2000 <= o < 0x12000 and n == SLOT_SIZE]


def slot_bytes(path, i, backup=False):
    data = path.read_bytes()[test_gci.HDR:]
    base = (0x12000 if backup else 0x2000) + i * SLOT_SIZE
    return data[base:base + SLOT_SIZE]


# The opening shared by the scenarios that start a new file: the title's PRESS START, the card check, the
# file select's slot `slot` (0-3, moved to with the stick from slot 0), the name (eight A presses on the first
# letter, then End) and its "Is this name OK?" (the cursor starts on No). The new file is saved at once
# (save kind 23) and the prologue starts.
# The cursor starts on the last slot used (the card header's), so `moves` take it from there.
def new_file_events(moves=(), create_card=True):
    ev = [(950, "start", 3)]
    if create_card:
        ev += [(1600, "a", 3), (2150, "a", 3)]  # "Create a Save File now?" Yes; "A Save File has been created."
        t = 2250
    else:
        ev += [(1400, "a", 3)]  # "The Save File has been loaded."
        t = 1560
    for i, k in enumerate(moves):
        ev.append((t - 60 + 20 * i, k, 3))
    ev.append((t, "a", 3))  # New Game
    ev += [(t + 250 + 12 * i, "a", 2) for i in range(8)]
    ev += [(t + 370, "a", 2), (t + 440, "cu", 3), (t + 480, "a", 3)]
    return ev


def run_scenario(build, sc):
    dest = OUT / sc["name"]
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    prepare_profile(sc["card_in"])
    last = dest / "azahar"
    rc, out, log, secs = run_game(build, sc["switches"], sc["pad"], sc["until"], sc["timeout"], last)
    shots = collect_shots(dest / "shots")
    card = GAME_DIR / "card" / SAVE_GCI
    if card.exists():
        shutil.copy(card, dest / SAVE_GCI)
    ctx = Ctx(sc["name"], log, out, rc, shots, card if card.exists() else None)
    tail = (out.strip().splitlines() or [""])[-1]
    ctx.check(rc == 0, f"run_azahar: {tail}")
    bad = error_lines(log)
    ctx.check(not bad, "no crash or error lines in the game log" + (f" (first: {bad[0].strip()})" if bad else ""))
    try:
        sc["check"](ctx)
    except Exception as e:  # a check that cannot read what it needs fails the scenario
        ctx.check(False, f"check raised {type(e).__name__}: {e}")
    ok = all(r for r, _ in ctx.results)
    if ok and sc["card_out"] and ctx.card is not None:
        CARDS.mkdir(parents=True, exist_ok=True)
        shutil.copy(ctx.card, CARDS / f"{sc['card_out']}.gci")
    return ok, ctx.results, secs


# ---- the scenarios ----

def check_card(ctx, used):
    """The card on the SD card: a valid GCI, big-endian, header checksum good, exactly the slots in `used` hold
    saves with both copies valid."""
    if not ctx.check(ctx.card is not None, f"the save file {SAVE_GCI} exists"):
        return None
    g = card_state(ctx.card)
    ctx.check(not g.errors, "the GCI entry is valid" + (f": {'; '.join(g.errors)}" if g.errors else ""))
    ctx.check(g.endian == "BE" and g.header_ok, f"save data big-endian with a good header (endian {g.endian})")
    for i in range(4):
        a, b, flags = g.slots[i]
        if i in used:
            ctx.check(a and b and not flags & 1, f"slot {i + 1} holds a save, main and backup valid (flags 0x{flags:x})")
        else:
            ctx.check(flags & 1, f"slot {i + 1} is empty (flags 0x{flags:x})")
    return g


def save_gw(ctx):
    """The last field gwdump record before the soft reset, kept for reset_load to compare with."""
    f, r = last_gw(ctx.log, lambda r: r.get("seq") == "2" and "mario" in r)
    return f, r


NEW_T = 2250  # new_file_events' New Game press with a card to create
# A every 100 frames: the new-file save's messages, then (Mario placed under the block) the jump into the save
# block, its first-time tutorial, "Save all game progress up to this point?" Yes, "Your progress was saved."
SAVE_NEW_PAD = pad(new_file_events() + [(NEW_T + 700 + 100 * i, "a", 3) for i in range(16)] +
                   [(NEW_T + 2400, "b+x+start", 60)])


@scenario("save_new", card_out="slot1", until=r"OSResetSystem", timeout=1500,
          switches="warpingame map=gor_01 story=30 marioat=185,0,-55 marioatwait=60 shots=20 shotfrom=1900",
          pad=SAVE_NEW_PAD,
          doc="fresh card: create the save file, new game in slot 1, warp to gor_01, hit the save block, soft reset")
def _save_new(ctx):
    log = ctx.log
    ctx.check(re.search(r"card: create mariost_save_file", log), "the card check created the save file")
    path = [(a, b, m) for a, b, f, m in seq_path(log)]
    ctx.check((6, 3, "") in path or any(a == 6 and b == 3 for a, b, _ in path), "the file select started a game")
    ctx.check(re.search(r"warp: gor_01 .*\(switch\)", log), "warped to gor_01 from the new file")
    ctx.check("sys: marioat" in log, "Mario placed under the save block")
    w = slot_writes(log)
    ctx.check(w.count(0) >= 2 and set(w) == {0}, f"slot 1 written by the new file and by the save block (slots {w})")
    ctx.check("OSResetSystem" in log, "B+X+START held: the soft reset ran (OSResetSystem, the port quits)")
    f, r = save_gw(ctx)
    if ctx.check(r is not None and r.get("map") == "gor_01" and r.get("gsw0") == "30",
                 f"in the field on gor_01 at story 30 before the reset (f={f})"):
        (OUT / "save_new_gw.json").write_text(json.dumps({"frame": f, **r}))
    check_card(ctx, {0})


LOAD_PAD = pad([(950, "start", 3), (1400, "a", 3), (1500, "a", 3), (1600, "a", 3)])


@scenario("reset_load", card_in="slot1", until=until_frame(1890), timeout=900, switches="shots=30 shotfrom=1440",
          pad=LOAD_PAD, doc="after the soft reset: load slot 1 from the file select; map, position and flags as saved")
def _reset_load(ctx):
    log = ctx.log
    ctx.check(re.search(r"sys: seq 6 -> 3 at frame \d+ map gor_01", log), "the file select continued the game into gor_01")
    f, r = last_gw(log, lambda r: r.get("seq") == "2" and "mario" in r)
    saved = json.loads((OUT / "save_new_gw.json").read_text())
    if not ctx.check(r is not None, "a field gwdump record after the load"):
        return
    ctx.check(r["map"] == "gor_01" and r["gsw0"] == "30", f"map {r['map']}, story {r['gsw0']} (saved: gor_01, 30)")
    x, y, z = (fbits(v) for v in r["mario"].split(","))
    sx, sy, sz = (fbits(v) for v in saved["mario"].split(","))
    # The map change puts Mario at the saved position (seq_mapchange.c, savePlayerPos, checked in the GCI); by the
    # first record after the load he stands 5 units to the -x side of it (open question, docs "Systems tests").
    ctx.check(abs(x - sx) <= 10 and abs(z - sz) <= 10 and abs(y - sy) < 1,
              f"Mario at {x:.1f},{y:.1f},{z:.1f}, within 10 units of the save (saved at {sx:.1f},{sy:.1f},{sz:.1f})")
    for k in ("gswf", "gsw"):
        ctx.check(r[k] == saved[k], f"story flags {k} hash {r[k]} equals the saved game's ({saved[k]})")
    ctx.check(not card_writes(log), "loading wrote nothing to the card")


# New files in slots 2, 3 and 4, one run each: the file select cursor starts on the last slot used.
def slot_scenario(n, moves, card_in, card_out):
    t = 1560
    ev = new_file_events(moves, create_card=False) + [(t + 900, "b+x+start", 60)]

    @scenario(f"save_slot{n + 1}", card_in=card_in, card_out=card_out, until=r"OSResetSystem", timeout=900,
              switches=f"shots=60 shotfrom=1400", pad=pad(ev),
              doc=f"new game in slot {n + 1} (its new-file save), soft reset")
    def _check(ctx):
        w = slot_writes(ctx.log)
        ctx.check(w == [n], f"only slot {n + 1} written (slots written: {[i + 1 for i in w]})")
        ctx.check("OSResetSystem" in ctx.log, "soft reset")
        check_card(ctx, set(range(n + 1)))
    return _check


slot_scenario(1, ["cr"], "slot1", "slots12")
slot_scenario(2, ["cl", "cd"], "slots12", "slots123")
slot_scenario(3, ["cr"], "slots123", "slots1234")


# File select with four files, cursor on file 4: down to Copy Data, file 1, onto file 2 ("File 2 will be erased. OK to
# overwrite this data?" starts on No: up, A), "Data was copied" A; then left to Erase Data, down to file 3, A, the
# "Erase File 3?" prompt (No first: up, A), "File 3 was erased." A.
COPY_DELETE_PAD = pad([(950, "start", 3), (1400, "a", 3), (1520, "cd", 3), (1560, "a", 3), (1640, "a", 3),
                       (1700, "a", 3), (1880, "cu", 3), (1920, "a", 3), (2300, "a", 3), (2450, "cl", 3),
                       (2490, "a", 3), (2570, "cd", 3), (2630, "a", 3), (2770, "cu", 3), (2810, "a", 3),
                       (3150, "a", 3)])


@scenario("copy_delete", card_in="slots1234", until=until_frame(3360), timeout=900,
          switches="shots=20 shotfrom=1440", pad=COPY_DELETE_PAD,
          doc="file select: copy file 1 onto file 2, erase file 3")
def _copy_delete(ctx):
    w = slot_writes(ctx.log)
    ctx.check(w == [1, 2], f"slot 2 written (the copy), then slot 3 (the erase) (slots written: {[i + 1 for i in w]})")
    g = check_card(ctx, {0, 1, 3})
    if g is None:
        return
    before = CARDS / "slots1234.gci"
    for backup in (False, True):
        ctx.check(slot_bytes(ctx.card, 1, backup) == slot_bytes(ctx.card, 0, backup),
                  f"file 2 is a byte copy of file 1 ({'backup' if backup else 'main'} copy)")
    ctx.check(slot_bytes(ctx.card, 0) == slot_bytes(before, 0) and slot_bytes(ctx.card, 3) == slot_bytes(before, 3),
              "files 1 and 4 unchanged")
    ctx.check(slot_bytes(before, 1) != slot_bytes(before, 0), "(file 2 differed from file 1 before the copy)")
    for f, what in ((1460, "the four files"), (2400, "after the copy"), (3260, "after the erase")):
        shot = shot_near(ctx, f, 40)
        ctx.check(shot, f"file select shot {what}: {shot.relative_to(OUT) if shot else 'missing'}")


@scenario("game_over", until=until_frame(2280), timeout=900,
          switches="map=hei_01 story=30 party=1 battle=4 btlauto=fail hp=1 shots=30 shotfrom=1500",
          doc="Mario at 1 HP loses the hei_01 Goomba battle (no guards): GAME OVER, then back to the title")
def _game_over(ctx):
    seqs = seq_path(ctx.log)
    path = [(a, b) for a, b, f, m in seqs]
    ctx.check("sys: hp set to 1" in ctx.log, "Mario's HP set to 1")
    ctx.check(re.search(r"battle: start 4 ", ctx.log), "the battle started")
    ctx.check((4, 5) in path, "the battle ended in the game over sequence (4 -> 5)")
    ctx.check((5, 1) in path, "the game over returned to the title by itself (5 -> 1)")
    over = [f for a, b, f, m in seqs if (a, b) == (4, 5)]
    shots = [(p, im) for p, im in ctx.shots if over and over[0] + 120 <= int(p.stem) <= over[0] + 240]
    ctx.check(shots and all(not_black(im) for p, im in shots),
              f"GAME OVER screen shots drawn ({', '.join(p.name for p, im in shots)})")


def win_states(log):
    """The pause menu lines of syslog: [(frame, {active, tab, item, mode, badge, log, party, mail})], closed as None"""
    out = []
    for l in sys_lines(log):
        m = re.match(r"win open active (-?\d+) tab (-?\d+) item (-?\d+)/(-?\d+) badge (-?\d+) log (-?\d+) party (-?\d+) "
                     r"mail (-?\d+) at frame (\d+)", l)
        if m:
            v = [int(x) for x in m.groups()]
            out.append((v[8], dict(zip(("active", "tab", "item", "mode", "badge", "log", "party", "mail"), v[:8]))))
        m = re.match(r"win closed at frame (\d+)", l)
        if m:
            out.append((int(m.group(1)), None))
    return out


def shot_near(ctx, frame, within=40):
    """The first good shot taken in [frame, frame + within]"""
    for p, im in ctx.shots:
        if frame <= int(p.stem) <= frame + within and not_black(im):
            return p
    return None


TAB_NAMES = ["Mario", "Party", "Items", "Badges", "Journal"]


def check_tabs(ctx, ws):
    """Each of the five tabs was selected at the top level and entered (active 11 + tab id), with a shot of it."""
    for t, name in enumerate(TAB_NAMES):
        entered = [f for f, s in ws if s and s["tab"] == t and s["active"] == 11 + t]
        shot = shot_near(ctx, entered[0] + 20, 60) if entered else None
        ctx.check(entered and shot, f"{name} tab opened (frame {entered[0] if entered else '-'}), shot "
                                    f"{shot.relative_to(OUT) if shot else 'missing'}")


# Open the menu, enter each tab with A and leave it with B, move right to the next; B closes the menu.
PAUSE_PAD = pad([(1000, "start", 3), (1100, "a", 3), (1200, "b", 3), (1260, "cr", 3), (1340, "a", 3), (1440, "b", 3),
                 (1500, "cr", 3), (1580, "a", 3), (1660, "b", 3), (1740, "cr", 3), (1820, "a", 3), (1900, "b", 3),
                 (1960, "cr", 3), (2040, "a", 3), (2140, "b", 3), (2200, "cr", 3), (2260, "b", 3)])


@scenario("pause_tabs", until=until_frame(2400), timeout=900,
          switches="map=gor_01 story=30 party=1 shots=20 shotfrom=1000", pad=PAUSE_PAD,
          doc="gor_01, story 30: open the pause menu, enter and leave each of the five tabs, close it")
def _pause_tabs(ctx):
    ws = win_states(ctx.log)
    ctx.check(ws and ws[0][1] is not None, "the menu opened with START")
    check_tabs(ctx, ws)
    tabs = [s["tab"] for f, s in ws if s and s["active"] == 10]
    ctx.check(tabs[-1:] == [0], f"moving right from Journal wraps to Mario (top-level tabs in order: {tabs})")
    ctx.check(ws[-1][1] is None, "B closed the menu")


# Story jump to 30 (Petalburg gatekeeper's mail 0x10 and 0x1A included): Items > important items > Mailbox SP >
# the first mail, read it, back out; then the Journal's Map, Crystal Stars, Tattle Log and Badges pages.
MAIL_PAD = pad([(1000, "start", 3), (1100, "a", 3), (1200, "b", 3), (1260, "cr", 3), (1340, "a", 3), (1440, "b", 3),
                (1500, "cr", 3), (1580, "a", 3), (1660, "cd", 3), (1720, "a", 3), (1800, "a", 3), (1880, "a", 3),
                (1980, "b", 3), (2040, "b", 3), (2100, "b", 3), (2160, "b", 3), (2240, "cr", 3), (2320, "a", 3),
                (2400, "b", 3), (2460, "cr", 3), (2540, "a", 3), (2620, "a", 3), (2720, "b", 3), (2780, "cd", 3),
                (2840, "a", 3), (2920, "b", 3), (2980, "cd", 3), (3040, "a", 3), (3120, "b", 3), (3180, "cd", 3),
                (3240, "a", 3), (3320, "b", 3), (3380, "b", 3), (3440, "b", 3)])


@scenario("mail_journal", until=until_frame(3540), timeout=1200,
          switches="map=gor_01 story=30 storyjump party=1 shots=20 shotfrom=1000", pad=MAIL_PAD,
          doc="story jump to 30 (two mails): read a mail with the Mailbox SP, open each Journal page")
def _mail_journal(ctx):
    m = re.search(r"sys: story jump to 30: (\d+) setups run, mail (\d+)", ctx.log)
    ctx.check(m and int(m.group(2)) == 2, f"the story jump ran the setups and delivered 2 mails ({m and m.group(0)})")
    ws = win_states(ctx.log)
    check_tabs(ctx, ws)
    mail_list = [f for f, s in ws if s and s["item"] == 200 and s["mode"] == 1]
    mail_read = [f for f, s in ws if s and s["item"] in (201, 202)]
    shot = shot_near(ctx, mail_read[0] + 20, 80) if mail_read else None
    ctx.check(mail_list and [s for f, s in ws if s and s["mail"] == 2],
              f"the Mailbox SP's mail list opened with 2 mails (frame {mail_list[0] if mail_list else '-'})")
    ctx.check(mail_read and shot, f"a mail opened for reading, shot {shot.relative_to(OUT) if shot else 'missing'}")
    pages = sorted({s["log"] for f, s in ws if s and s["log"] >= 10})
    ctx.check(len(pages) >= 4, f"four Journal pages opened (log states {pages}: 10 map, 11 Crystal Stars, "
                               "12 Badges, 14 Tattle Log)")
    for pg in pages:
        f = [f for f, s in ws if s and s["log"] == pg][0]
        shot = shot_near(ctx, f + 20, 60)
        ctx.check(shot, f"Journal page {pg} shot {shot.relative_to(OUT) if shot else 'missing'}")
    ctx.check(ws[-1][1] is None, "B closed the menu")


def not_black(im):
    if im is None:
        return True
    return max(im.convert("L").getextrema()) > 40


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="scenarios to run (default all), or 'adhoc'")
    ap.add_argument("--build", type=Path, default=ROOT / "build/3ds/game/ttyd.cia")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--switches", default="", help="adhoc: game switches")
    ap.add_argument("--padscript", default="", help="adhoc: padscript, or @python-expr using pad() (frames)")
    ap.add_argument("--card", help="adhoc: start from this card fixture")
    ap.add_argument("--until", help="adhoc: run_azahar --until regex")
    ap.add_argument("--timeout", type=float, default=600)
    args = ap.parse_args()
    if args.list:
        for sc in SCENARIOS:
            print(f"{sc['name']:14s} card {sc['card_in'] or 'fresh'} -> {sc['card_out'] or '-'}: {sc['doc']}")
        return 0
    if not args.build.is_file():
        sys.exit(f"systems_test: {args.build} not found (tools/port3ds/build.sh)")
    if args.names == ["adhoc"]:
        dest = OUT / "adhoc"
        shutil.rmtree(dest, ignore_errors=True)
        dest.mkdir(parents=True)
        prepare_profile(args.card)
        script = args.padscript
        if script.startswith("@"):
            script = eval(script[1:], globals())
        rc, out, log, secs = run_game(args.build, args.switches, script, args.until, args.timeout,
                                      dest / "azahar")
        shots = collect_shots(dest / "shots")
        card = GAME_DIR / "card" / SAVE_GCI
        if card.exists():
            shutil.copy(card, dest / SAVE_GCI)
            print(test_gci.describe(card, card_state(card)))
        print(out.strip().splitlines()[-1] if out.strip() else "")
        for l in sys_lines(log):
            print("  sys:", l)
        for l in log.splitlines():
            if "card:" in l or "OSResetSystem" in l:
                print(" ", l)
        print(f"adhoc: {secs:.0f}s, {len(shots)} shots in {dest / 'shots'}")
        return rc
    todo = [s for s in SCENARIOS if not args.names or s["name"] in args.names]
    unknown = set(args.names) - {s["name"] for s in SCENARIOS}
    if unknown:
        sys.exit(f"systems_test: unknown scenario(s) {', '.join(sorted(unknown))} (--list)")
    OUT.mkdir(parents=True, exist_ok=True)
    res_path = OUT / "results.json"
    results = json.loads(res_path.read_text()) if res_path.exists() else {}
    failed = 0
    for sc in todo:
        print(f"systems_test: {sc['name']} ...", flush=True)
        try:
            ok, checks, secs = run_scenario(args.build, sc)
        except FileNotFoundError as e:
            ok, checks, secs = False, [(False, str(e))], 0
        for r, text in checks:
            print(f"  {'ok  ' if r else 'FAIL'} {text}")
        print(f"systems_test: {sc['name']}: {'PASS' if ok else 'FAIL'} ({secs:.0f}s)", flush=True)
        results[sc["name"]] = {"pass": ok, "checks": [[r, t] for r, t in checks], "seconds": round(secs)}
        failed += not ok
        res_path.write_text(json.dumps(results, indent=1))
    print(f"systems_test: {len(todo) - failed} of {len(todo)} scenarios passed; details in {OUT}/")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
