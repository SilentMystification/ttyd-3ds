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


# Trouble Center (gor_02, house 04), story 30, its lecture marked as heard (GSWF 5374). Mario opens the door,
# is put in front of the board (A_i04_keizi), walks up to it, A: the trouble list (no mash meanwhile), stick down
# twice, A: trouble 3 (the Fire Flower price check), "Accept?" Yes: GSW 1420 (active trouble) = 3. Then he is put
# in front of the third stall seller (NPC slot 9), A: the trouble's talk, four choices, 20 coins, GSWF 5377 (solved).
@scenario("trouble", "map=gor_02 story=30 party=1 gswf=5374 swwatch=1420,f5377 mash=10 nomash=1060-1220 "
          "marioat=hit:A_ie_04_doa01,-29,0,30 marioat2=hit:A_i04_keizi,-84,0,31@1000 marioat3=hit:npc9,0,0,40@1700 "
          "shots=60 shotfrom=1060",
          pad=st.pad([(710, "cu", 4), (720, "a", 3), (1030, "cu", 15), (1060, "a", 3), (1120, "cd", 3),
                      (1140, "cd", 3), (1160, "a", 3), (1730, "cu", 3), (1740, "a", 3)]),
          until=until_frame(2250), doc="Trouble Center: accept trouble 3 at the board, solve it, 20 coins")
def _trouble(ctx):
    seen(ctx, "irai_help_03", "trouble 3's description (picked from the list)")
    chose(ctx, "irai_01", 0, "(accept: Yes) ")
    active = watched(ctx.log, "gsw 1420")
    ctx.check([v for f, v in active] == [0, 3, 0], f"active trouble 0 -> 3 -> 0 (GSW 1420: {active})")
    seen(ctx, "irai_03_00", "the seller's trouble talk")
    solved = watched(ctx.log, "gswf 5377")
    ctx.check(solved and solved[-1][1] == 1, f"trouble 3 solved (GSWF 5377: {solved})")
    coins = values(ctx.log, "coins")
    ctx.check(coins and coins[-1][1] == 120, f"20 coins reward: 100 -> {last(coins)}")
    seen(ctx, "sys_request_finish", "'Trouble solved'")
    shot = shot_after(ctx, 1080, 120)
    ctx.check(shot, f"shot of the trouble list: {rel(shot)}")


MOT_STAY, MOT_SLIT, MOT_ROLL, MOT_PLANE, MOT_SHIP = 0x0, 0x15, 0x16, 0x18, 0x19
# the stick turned in a circle in 45-degree steps (2 frames each) with R held: marioGetRub counts turns
CIRCLE = ["cu", "cu+cr", "cr", "cr+cd", "cd", "cd+cl", "cl", "cl+cu"]


def circles(t, n):
    ev = []
    for c in range(n):
        for k in CIRCLE:
            ev.append((t, "r+" + k, 2))
            t += 2
    return ev, t


def motions(ctx):
    return [(f, v) for f, v in values(ctx.log, "motion")]


def motion_after(ctx, want, frame=0):
    f = [fr for fr, v in motions(ctx) if v == want and fr >= frame]
    return f[0] if f else None


_tube, _t = circles(820, 5)
# Paper (R held: thin paper Mario) and tube (paper, then R held while the stick turns three circles; B unrolls).
# The Paper and Tube Curses are given; open ground in gor_02.
@scenario("paper_tube", "map=gor_02 story=30 party=1 items=2,3 marioat=-150,0,50 shots=30 shotfrom=720",
          pad=st.pad([(720, "r", 30)] + _tube + [(_t, "r", 10), (980, "b", 3)]), until=until_frame(1110),
          doc="transformations: paper (R) and tube (R + stick circles), back to standing")
def _paper_tube(ctx):
    p = motion_after(ctx, MOT_SLIT)
    ctx.check(p is not None and p <= 725, f"R held at frame 720: paper mode (motion 0x15) at {p}")
    back = motion_after(ctx, MOT_STAY, p or 0)
    ctx.check(back is not None and back < 820, f"R released: standing again (motion 0) at {back}")
    t = motion_after(ctx, MOT_ROLL)
    ctx.check(t is not None and 820 < t < 900, f"R + stick circles: tube mode (motion 0x16) at {t}")
    end = motion_after(ctx, MOT_STAY, t or 10 ** 9)
    ctx.check(end is not None and end > 980, f"B: unrolled, standing again at {end}")
    for f, what in ((p, "paper"), (t, "tube")):
        shot = shot_after(ctx, (f or 0) + 10, 40)
        ctx.check(shot, f"{what} shot: {rel(shot)}")


# Boat: the Boat Curse given, Mario put on the Rogueport harbour's boat panel (gor_02 hit Ship_Change01), Y: the
# curse's transformation scene, then the paper boat on the water; B (pressed every 20 frames) docks him at the panel.
@scenario("boat", "map=gor_02 story=30 party=1 items=5 marioat=hit:Ship_Change01,65 shots=30 shotfrom=720",
          pad=st.pad([(720, "y", 3)] + [(880 + 20 * i, "b", 3) for i in range(12)]), until=until_frame(1200),
          doc="transformation: boat at the gor_02 boat panel (Y), docked again (B)")
def _boat(ctx):
    s = motion_after(ctx, MOT_SHIP)
    ctx.check(s is not None and s <= 725, f"Y on the panel: boat (motion 0x19) at {s}")
    end = motion_after(ctx, MOT_STAY, (s or 0) + 1)
    ctx.check(end is not None and end > 880, f"B at the panel: docked, standing at {end}")
    shot = shot_after(ctx, 960, 60)
    ctx.check(shot, f"boat shot: {rel(shot)}")


# Plane: the Plane Curse given, Mario put on Hooktail Castle's plane panel (gon_01 hit a_hi; story 60, after the
# castle's opening scene), Y: the transformation scene, the glide, the landing (standing again).
@scenario("plane", "map=gon_01 story=60 party=1 items=4 marioat=hit:a_hi,10 shots=30 shotfrom=720",
          pad=st.pad([(720, "y", 3)]), until=until_frame(1110),
          doc="transformation: paper plane from the gon_01 plane panel (Y), glide and landing")
def _plane(ctx):
    s = motion_after(ctx, MOT_PLANE)
    ctx.check(s is not None and s <= 725, f"Y on the panel: plane (motion 0x18) at {s}")
    end = motion_after(ctx, MOT_STAY, (s or 0) + 1)
    ctx.check(end is not None and end > (s or 0) + 120, f"landed, standing at {end}")
    shot = shot_after(ctx, 930, 60)
    ctx.check(shot, f"glide shot: {rel(shot)}")


# Pianta Parlor Plane Game (yuu_00), warped in directly with 20 Piantas held (piantas=: as after paying at the
# parlor). The game turns Mario into the plane by itself; with no input he glides straight and lands. A on the two
# result screens, then "Play again?" (10 Piantas): down, A = quit, and the pipe takes him back to the parlor.
@scenario("parlor_plane", "map=yuu_00 story=30 party=1 piantas=20 mash=10 msgkeys=sysyuu_00_00:down.a "
          "shots=60 shotfrom=900", pad=st.pad([(1620 + 40 * i, "a", 3) for i in range(3)]), until=until_frame(1860),
          doc="Pianta Parlor Plane Game: one round, the result screens, quit back to gor_03")
def _parlor_plane(ctx):
    s = motion_after(ctx, MOT_PLANE)
    ctx.check(s is not None, f"the game made Mario a plane (motion 0x18 at {s})")
    land = motion_after(ctx, MOT_STAY, (s or 0) + 1)
    ctx.check(land is not None, f"landed at {land}")
    p = values(ctx.log, "piantas")
    ctx.check(len(p) >= 2 and p[-1][1] > 20, f"Piantas paid out after the round ({[v for f, v in p]})")
    chose(ctx, "sysyuu_00_00", 1, "(play again? No) ")
    ctx.check("yuu_00 -> gor_03" in map_changes(ctx.log), "back to the parlor (gor_03)")
    shot = shot_after(ctx, (s or 0) + 60, 80)
    ctx.check(shot, f"flight shot: {rel(shot)}")


# Paper Game (yuu_03), warped in with 20 Piantas: with no input the wind blows Mario off the course (a lost
# round), the result screens (mash), "Play again?" down, A = quit, back to the parlor.
@scenario("parlor_paper", "map=yuu_03 story=30 party=1 piantas=20 mash=10 msgkeys=sysyuu_03_00:down.a "
          "shots=60 shotfrom=960", until=until_frame(1500),
          doc="Pianta Parlor Paper Game: one (lost) round, quit back to gor_03")
def _parlor_paper(ctx):
    fall = motion_after(ctx, 0xA)
    ctx.check(fall is not None and fall > 900, f"blown off the course: falling (motion 0xa) at {fall}")
    seen(ctx, "sysyuu_03_00", "'Play again?'")
    chose(ctx, "sysyuu_03_00", 1, "(No) ")
    ctx.check("yuu_03 -> gor_03" in map_changes(ctx.log), "back to the parlor (gor_03)")
    shot = shot_after(ctx, 960, 60)
    ctx.check(shot, f"course shot: {rel(shot)}")


# Tube Game (yuu_02), warped in with 20 Piantas: Mario starts as the tube, rolls right for 200 frames, then START:
# the minigame pause window, "Quit?" (the cursor starts on No): up, A, and back to the parlor. Its round is a
# 180 s timer, too long for a short run, so the test quits through the pause window.
@scenario("parlor_tube", "map=yuu_02 story=30 party=1 piantas=20 mash=10 msgkeys=sysyuu_all_02:up.a "
          "shots=60 shotfrom=1100", pad=st.pad([(1100, "cr", 200), (1400, "start", 3)]), until=until_frame(1800),
          doc="Pianta Parlor Tube Game: roll, pause, quit back to gor_03")
def _parlor_tube(ctx):
    ctx.check(motion_after(ctx, MOT_ROLL) is not None, "Mario is the tube in the game (motion 0x16)")
    seen(ctx, "sysyuu_all_01", "the pause window")
    chose(ctx, "sysyuu_all_02", 0, "(quit) ")
    ctx.check("yuu_02 -> gor_03" in map_changes(ctx.log), "back to the parlor (gor_03)")
    back = [f for a, b, f, m in st.seq_path(ctx.log) if b == 2 and m == "gor_03"]
    ctx.check(back, f"gor_03 up in the field (frame {back[0] if back else '-'})")
    shot = shot_after(ctx, 1140, 60)
    ctx.check(shot, f"rolling shot: {rel(shot)}")


# Pit of 100 Trials: warp into jon_00 through its entry pipe (dokan_2), floor 1 (GSW 1321 = 0). Mario is put on
# the floor's enemy (NPC slot 0, a Gloomba): a first strike and the battle, run by btlauto=perfect. Back in the
# field the exit pipe rises (about 3 s); Mario is put on it (hit a_dokan_1), stick down: floor 2 (GSW 1321 = 1,
# deepest floor GSW 33 = 2), the same map loaded again, and floor 2's enemy starts its battle.
@scenario("pit", "map=jon_00,dokan_2 story=30 party=1 btlauto=perfect swwatch=1321,33 "
          "marioat2=hit:npc0,0@800 marioat3=hit:a_dokan_1,40@4400 shots=60 shotfrom=820",
          pad=st.pad([(4440, "cd", 30)]), until=until_frame(4800), timeout=900,
          doc="Pit of 100 Trials: floor 1 battle (btlauto), the pipe, floor 2 and its battle")
def _pit(ctx):
    path = st.seq_path(ctx.log)
    battles = [f for a, b, f, m in path if (a, b) == (2, 4) and m == "jon_00"]
    ends = [f for a, b, f, m in path if (a, b) == (4, 2) and m == "jon_00"]
    ctx.check(battles, f"floor 1 battle started (frame {battles[0] if battles else '-'})")
    ctx.check(re.search(r"btlauto: action command ends at frame \d+, result 2", ctx.log),
              "btlauto: action commands succeeded")
    ctx.check(ends, f"battle won, back on floor 1 (frame {ends[0] if ends else '-'})")
    floor = watched(ctx.log, "gsw 1321")
    ctx.check([v for f, v in floor] == [0, 1], f"the pipe: floor 1 -> 2 (GSW 1321: {floor})")
    deep = watched(ctx.log, "gsw 33")
    ctx.check(deep and deep[-1][1] == 2, f"deepest floor 2 (GSW 33: {deep})")
    reload = [f for a, b, f, m in path if (a, b) == (2, 3) and m == "jon_00"]
    ctx.check(reload, f"jon_00 loaded again for floor 2 (frame {reload[0] if reload else '-'})")
    ctx.check(len(battles) >= 2, f"floor 2's enemy: its battle started (frames {battles})")
    shot = shot_after(ctx, (battles[0] if battles else 0) + 200, 60)
    ctx.check(shot, f"battle shot: {rel(shot)}")


# Deeper floors of the Pit: the same entry as "pit" with GSW 1321 (the 0-based floor) set while the map loads, so
# the floor's own enemy (NPC slot 0) is fought: floor 11 on jon_00 (floors 1-49), floor 51 on jon_01 (51-79),
# floor 99 on jon_02 (81-99). Floors 10, 20, ... 100 are rest rooms or the boss and have no enemy in slot 0.
def _pit_deep(name, mp, floor, win=True, timeout=900):
    @scenario(name, f"map={mp},dokan_2 story=30 party=1 btlauto=perfect gsw=1321:{floor - 1} swwatch=1321,33 "
              f"marioat2=hit:npc0,0@800 shots=60 shotfrom=820",
              until=r"sys: seq 4 -> 2 at frame" if win else r"btlauto: action command ends at frame \d+, result 2",
              timeout=timeout,
              doc=f"Pit of 100 Trials: floor {floor} ({mp}), one battle (btlauto)")
    def _check(ctx):
        path = st.seq_path(ctx.log)
        battles = [f for a, b, f, m in path if (a, b) == (2, 4) and m == mp]
        ends = [f for a, b, f, m in path if (a, b) == (4, 2) and m == mp]
        floor_now = watched(ctx.log, "gsw 1321")
        ctx.check(floor_now and floor_now[-1][1] == floor - 1, f"warped to floor {floor} (GSW 1321: {floor_now})")
        ctx.check(battles, f"floor {floor} battle started (frame {battles[0] if battles else '-'})")
        ctx.check(re.search(r"btlauto: action command ends at frame \d+, result 2", ctx.log),
                  "btlauto: action commands succeeded")
        if win:
            ctx.check(ends, f"battle won, back on the floor (frame {ends[0] if ends else '-'})")
        shot = shot_after(ctx, (battles[0] if battles else 0) + 200, 60)
        ctx.check(shot, f"battle shot: {rel(shot)}")


_pit_deep("pit11", "jon_00", 11)
_pit_deep("pit51", "jon_01", 51, win=False)
_pit_deep("pit99", "jon_02", 99, win=False)


# Bunny Teresa's table (gor_03, the cage at the back of the Pianta Parlor hall), plane game unlocked (GSWF 1232) and
# 20 Piantas. The parlor has no floor until its front door opens, so Mario is put at the door (marioat2), walks up
# (stick up 300 frames) and opens it with A; then he is put on the mat in front of the cage (hit:npc11, dx -70),
# turns to her (stick up) and A talks: greeting, the game menu (first entry: the plane game), "10 Piantas?" (Yes),
# the fee is paid. Then Teresa raises the pipe and Mario walks into it (yuu_00); the plane round is
# parlor_plane's. KNOWN FAILURE (2026-11-26): after the fee bubble (bunny_teresa_36) the script stalls for
# 3000+ frames (no pipe, Mario standing), so the last checks fail until that is understood.
@scenario("parlor_teresa", "map=gor_03 story=30 party=1 piantas=20 gswf=1232 mash=10 msgkeys=sysyuu_00_00:down.a "
          "marioat2=-200,30,40@800 marioat3=hit:npc11,0,-70,0@1450 shots=60 shotfrom=1500",
          pad=st.pad([(900, "cu", 300), (1230, "a", 3), (1500, "cu", 8), (1540, "a", 3)]),
          until=until_frame(3510), timeout=900,
          doc="Bunny Teresa: talk, pick the plane game, pay 10 Piantas, the pipe, the round")
def _parlor_teresa(ctx):
    seen(ctx, "bunny_teresa_00", "Teresa's greeting")
    chose(ctx, "bunny_teresa_yesno2", 0, "(Yes) ")
    p = values(ctx.log, "piantas")
    ctx.check(len(p) >= 3 and p[2][1] == 10 and p[-1][1] > 10, f"fee paid 20 -> 10, then the round paid out ({[v for f, v in p]})")
    ctx.check("gor_03 -> yuu_00" in map_changes(ctx.log), "down the pipe into the plane game (yuu_00)")
    ctx.check("yuu_00 -> gor_03" in map_changes(ctx.log), "round played, back at the parlor (gor_03)")


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
