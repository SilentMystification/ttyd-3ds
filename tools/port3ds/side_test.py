#!/usr/bin/env python3
"""Side content tests in Azahar (backlog lane B item 5): the Pit of 100 Trials, the Trouble Center, the Pianta
Parlor, the Shhwonk Fortress quiz, cooking with Zess T., shops and Mario's transformations. The list is in
docs/PORT_3DS_TESTING.md, "Side content tests".

    tools/port3ds/side_test.py                   # every scenario, in order
    tools/port3ds/side_test.py quiz shop         # some of them
    tools/port3ds/side_test.py --list
    tools/port3ds/side_test.py adhoc --switches "map=hei_07 story=30 shots=60" --padscript "..." --until "gwdump f=1200 "

Same machinery as systems_test.py (one run_azahar.py run per scenario, headless, under the Azahar slot lock,
`determ`, `syslog`, `gwdump=30`), with its own profile (build/side-profile/, no save file) and results
(build/side-test/<scenario>/, results.json). A scenario starts a new game on a map (`map=`, `story=`), sets up
items, coins and flags (`items=`, `coins=`, `gsw=`, `gswf=`, `piantas=`), places Mario (`marioat=`, also on a hit
object by name), walks him with a padscript and lets the dialogue autopilot (`mash=`, `msgkeys=`) answer the
windows by their message tags. Checks read the `sys:` lines: the message tags shown, choices made, coins, held
items, Mario's motion, map changes and watched story flags. Exit status 0 when every scenario passes.
"""
import argparse
import json
import re
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "port3ds"))
import systems_test as st  # noqa: E402

# systems_test's machinery on this tool's own profile and output folders
st.PROFILE = ROOT / "build" / "side-profile"
st.SDMC = st.PROFILE / "data" / "azahar-emu" / "sdmc"
st.GAME_DIR = st.SDMC / "3ds" / "ttyd"
st.OUT = OUT = ROOT / "build" / "side-test"

# No update server: another agent's devupdate push cannot reach this instance.
BASE = "nodevupdate"


# ---- log parsing (the sys: lines of test_systems.c) ----

def sys_lines(log):
    return st.sys_lines(log)


def msgs(log):
    """[(frame, tag, choice)] in order"""
    out = []
    for l in sys_lines(log):
        m = re.match(r"msg (\S+)( \(choice\))? at frame (\d+)", l)
        if m:
            out.append((int(m.group(3)), m.group(1), bool(m.group(2))))
    return out


def tags(log):
    return [t for f, t, c in msgs(log)]


def selects(log):
    """[(frame, tag, choice index)]"""
    out = []
    for l in sys_lines(log):
        m = re.match(r"select (\S+) -> (-?\d+) at frame (\d+)", l)
        if m:
            out.append((int(m.group(3)), m.group(1), int(m.group(2))))
    return out


def values(log, what):
    """[(frame, value)] of a 'sys: <what> V at frame F' line (coins, piantas, gsw0, motion)"""
    out = []
    for l in sys_lines(log):
        m = re.match(rf"{what} (\S+) at frame (\d+)", l)
        if m:
            out.append((int(m.group(2)), int(m.group(1), 0)))
    return out


def items(log):
    """[(frame, [item ids])] of the held items"""
    out = []
    for l in sys_lines(log):
        m = re.match(r"items \[([0-9a-f,]*)\] at frame (\d+)", l)
        if m:
            out.append((int(m.group(2)), [int(x, 16) for x in m.group(1).split(",") if x]))
    return out


def watched(log, name):
    """[(frame, value)] of a swwatch entry: name 'gsw 1321' or 'gswf 5377'"""
    out = []
    for l in sys_lines(log):
        m = re.match(rf"{name} = (-?\d+) at frame (\d+)", l)
        if m:
            out.append((int(m.group(2)), int(m.group(1))))
    return out


def map_changes(log):
    """['from -> to (entrance bero)'] from the map: lines"""
    return [m.group(1) for m in re.finditer(r"map: (\S+ -> \S+)", log)]


def last(seq, default=None):
    return seq[-1][1] if seq else default


def shot_after(ctx, frame, within=120):
    return st.shot_near(ctx, frame, within)


def rel(p):
    return p.relative_to(OUT) if p else "missing"


# ---- scenarios ----

SCENARIOS = []


def scenario(name, switches, pad="", until=None, timeout=600, doc=""):
    def deco(fn):
        SCENARIOS.append(dict(name=name, card_in=None, card_out=None, timeout=timeout, until=until,
                              switches=f"{BASE} {switches}", pad=pad, check=fn, doc=doc))
        return fn
    return deco


def until_frame(n):
    return st.until_frame(n)


def seen(ctx, tag, what=None):
    """A check that a message tag was shown"""
    f = [fr for fr, t, c in msgs(ctx.log) if t == tag]
    return ctx.check(f, f"{what or 'message'} {tag} shown" + (f" (frame {f[0]})" if f else ""))


def chose(ctx, tag, want, what=""):
    s = [(fr, n) for fr, t, n in selects(ctx.log) if t == tag]
    return ctx.check(s and s[0][1] == want, f"choice {tag} answered {want} {what}(got {[n for f, n in s]})")


# Shhwonk Fortress (hei_07), story 30 (before "Quiz"): Mario stands left of the Thwomp statue, steps right, A.
# The statue wakes, asks "take the quiz?" (Yes, the default), question 1 (answer 0, the default, is right),
# "Is that your final answer?" (Yes), "right!", question 2: the D-pad moves the cursor to answer 2 (right).
@scenario("quiz", "map=hei_07,w_bero story=30 party=1 mash=10 marioat=hit:a_sekizou_1_a,-54,-45,0 "
          "msgkeys=stg1_hei_q_02:down.down.a shots=100 shotfrom=1100",
          pad=st.pad([(700, "cr", 20), (740, "a", 3)]), until=until_frame(2880),
          doc="Shhwonk Fortress quiz (the Thwomp statue): accept, two questions answered right")
def _quiz(ctx):
    seen(ctx, "stg1_hei_20", "the statue's greeting")
    chose(ctx, "stg1_hei_yn_01", 0, "(take the quiz: Yes) ")
    seen(ctx, "stg1_hei_23", "question 1")
    chose(ctx, "stg1_hei_q_01", 0, "(right answer 0) ")
    seen(ctx, "stg1_hei_24", "question 2")
    chose(ctx, "stg1_hei_q_02", 2, "(right answer 2, cursor moved with the D-pad) ")
    right = [f for f, t, c in msgs(ctx.log) if t == "stg1_hei_33"]
    ctx.check(len(right) >= 2, f"'right answer' (stg1_hei_33) after both questions (frames {right})")
    wrong = [t for t in tags(ctx.log) if t in ("stg1_hei_36", "stg1_hei_37", "stg1_hei_38")]
    ctx.check(not wrong, f"no wrong-answer message ({wrong})")
    shot = shot_after(ctx, right[0] if right else 0, 200)
    ctx.check(shot, f"shot of the quiz: {rel(shot)}")


ITEM_MUSHROOM = 0x94

# Rogueport (gor_01) item shop in house 02, story 30, 100 coins; the shop and item tutorials marked as seen
# (GSWF 0, 235). Mario stands at the shop door, pushes up and presses A (the door opens, he walks in), is put on
# the Mushroom shelf (A_item_01), presses A: "Mushroom, 5 coins?" Yes. Then he is put in front of the counter,
# walks right to the shopkeeper (NPC slot 2), A: the counter menu, down to Sell, the item list (A: the Mushroom),
# "2 coins?" Yes.
@scenario("shop", "map=gor_01 story=30 party=1 gswf=0,235 mash=10 marioat=hit:A_ie_02_f1_doa01,-37,0,30 "
          "marioat2=hit:A_item_01,0@1000 marioat3=hit:npc2,0,0,60@1300 "
          "msgkeys=gor_shop1_24:a,gor_shop1_24:a,gor_shop1_24:down.a,gor_shop1_23:down.a shots=60 shotfrom=1000",
          pad=st.pad([(710, "cu", 4), (720, "a", 3), (1030, "cu", 3), (1040, "a", 3), (1330, "cr", 12),
                      (1350, "a", 3)]),
          until=until_frame(1800), doc="Rogueport shop: walk in, buy a Mushroom (5 coins), sell it back (2 coins)")
def _shop(ctx):
    coins = values(ctx.log, "coins")
    its = items(ctx.log)
    ctx.check(any(v == 100 for f, v in coins), f"100 coins after the warp ({[v for f, v in coins]})")
    buy = [(f, v) for f, v in coins if v == 95]
    ctx.check(buy, "the Mushroom bought: 100 -> 95 coins")
    ctx.check(any(ITEM_MUSHROOM in l for f, l in its), f"a Mushroom in the held items ({its})")
    chose(ctx, "gor_shop1_23", 1, "(the counter menu: Sell) ")
    ctx.check(coins and coins[-1][1] == 97, f"the Mushroom sold: 95 -> 97 coins (last {last(coins)})")
    ctx.check(its and its[-1][1] == [], f"no held item after the sale ({its[-1] if its else '-'})")
    shot = shot_after(ctx, buy[0][0] if buy else 0, 120)
    ctx.check(shot, f"shot in the shop: {rel(shot)}")


ITEM_SHROOM_FRY = 0xB3

# Zess T. (gor_01, house 01), story 30, the Contact Lens delivered (GSWF 1188: she cooks), a Mushroom given.
# Mario opens house 01's door (up, A), is put in front of her (NPC slot 4), up, A: her greeting, "Cook?" Yes,
# the item list (A: the Mushroom), "Cook the Mushroom?" Yes, the cooking scene, the Shroom Fry.
@scenario("cook", "map=gor_01 story=30 party=1 gswf=1188 items=0x94 mash=10 marioat=hit:A_ie_01_doa01,-43,0,30 "
          "marioat2=hit:npc4,0,0,40@1000 shots=60 shotfrom=1000",
          pad=st.pad([(710, "cu", 4), (720, "a", 3), (1030, "cu", 3), (1040, "a", 3)]),
          until=until_frame(2550), doc="cooking with Zess T.: a Mushroom cooked into a Shroom Fry")
def _cook(ctx):
    its = items(ctx.log)
    ctx.check(any(l == [ITEM_MUSHROOM] for f, l in its), f"the Mushroom given ({its[:3]})")
    seen(ctx, "gor_01_cook_000", "Zess T.'s first-time greeting")
    sel = [n for f, t, n in selects(ctx.log) if t == "gor_01_select"]
    ctx.check(sel[:2] == [0, 0], f"'Cook?' and 'Cook the Mushroom?' answered Yes ({sel})")
    ctx.check(any(l == [] for f, l in its), "the Mushroom taken")
    got = [f for f, l in its if l == [ITEM_SHROOM_FRY]]
    ctx.check(got, f"a Shroom Fry (0xb3) in the held items (frame {got[0] if got else '-'})")
    seen(ctx, "gor_01_cook_023", "the closing message")
    shot = shot_after(ctx, got[0] if got else 0, 80)
    ctx.check(shot, f"shot of the dish: {rel(shot)}")


def run_scenario(build, sc):
    dest = OUT / sc["name"]
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    st.prepare_profile(None)
    rc, out, log, secs = st.run_game(build, sc["switches"], sc["pad"], sc["until"], sc["timeout"], dest / "azahar")
    shots = st.collect_shots(dest / "shots")
    ctx = st.Ctx(sc["name"], log, out, rc, shots, None)
    tail = (out.strip().splitlines() or [""])[-1]
    ctx.check(rc == 0, f"run_azahar: {tail}")
    bad = st.error_lines(log)
    ctx.check(not bad, "no crash or error lines in the game log" + (f" (first: {bad[0].strip()})" if bad else ""))
    try:
        sc["check"](ctx)
    except Exception as e:  # a check that cannot read what it needs fails the scenario
        ctx.check(False, f"check raised {type(e).__name__}: {e}")
    return all(r for r, _ in ctx.results), ctx.results, secs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="scenarios to run (default all), or 'adhoc'")
    ap.add_argument("--build", type=Path, default=ROOT / "build/3ds/game/ttyd.cia")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--switches", default="", help="adhoc: game switches")
    ap.add_argument("--padscript", default="", help="adhoc: padscript, or @python-expr using pad() (frames)")
    ap.add_argument("--until", help="adhoc: run_azahar --until regex")
    ap.add_argument("--timeout", type=float, default=600)
    args = ap.parse_args()
    if args.list:
        for sc in SCENARIOS:
            print(f"{sc['name']:12s} {sc['doc']}")
        return 0
    if not args.build.is_file():
        sys.exit(f"side_test: {args.build} not found (tools/port3ds/build.sh)")
    if args.names == ["adhoc"]:
        dest = OUT / "adhoc"
        shutil.rmtree(dest, ignore_errors=True)
        dest.mkdir(parents=True)
        st.prepare_profile(None)
        script = args.padscript
        if script.startswith("@"):
            script = eval(script[1:], {"pad": st.pad})
        rc, out, log, secs = st.run_game(args.build, f"{BASE} {args.switches}", script, args.until, args.timeout,
                                         dest / "azahar")
        shots = st.collect_shots(dest / "shots")
        print(out.strip().splitlines()[-1] if out.strip() else "")
        for l in log.splitlines():
            if "sys: " in l or "map: " in l or "msgkeys" in l or "battle: " in l or "warp: " in l:
                k = min(i for i in (l.find(w) for w in ("sys: ", "map: ", "msgkeys", "battle: ", "warp: "))
                        if i >= 0)
                print(" ", l[k:])
        for l in st.error_lines(log)[:5]:
            print("  ERROR:", l)
        print(f"adhoc: rc {rc}, {secs:.0f}s, {len(shots)} shots in {dest / 'shots'}")
        return rc
    todo = [s for s in SCENARIOS if not args.names or s["name"] in args.names]
    unknown = set(args.names) - {s["name"] for s in SCENARIOS}
    if unknown:
        sys.exit(f"side_test: unknown scenario(s) {', '.join(sorted(unknown))} (--list)")
    OUT.mkdir(parents=True, exist_ok=True)
    res_path = OUT / "results.json"
    results = json.loads(res_path.read_text()) if res_path.exists() else {}
    failed = 0
    for sc in todo:
        print(f"side_test: {sc['name']} ...", flush=True)
        ok, checks, secs = run_scenario(args.build, sc)
        for r, text in checks:
            print(f"  {'ok  ' if r else 'FAIL'} {text}")
        print(f"side_test: {sc['name']}: {'PASS' if ok else 'FAIL'} ({secs:.0f}s)", flush=True)
        results[sc["name"]] = {"pass": ok, "checks": [[r, t] for r, t in checks], "seconds": round(secs)}
        failed += not ok
        res_path.write_text(json.dumps(results, indent=1))
    print(f"side_test: {len(todo) - failed} of {len(todo)} scenarios passed; details in {OUT}/")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
