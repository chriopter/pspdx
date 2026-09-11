#!/usr/bin/env python3
"""Customers, written down as key scripts.

Nobody uses a package manager by pressing buttons at random: they come in
looking for something. A profile here is one of those errands -- someone who
only scrolls, someone who installs four things one at a time, someone who
goes straight to the updates tab and takes the lot -- turned into a
PSPDX.KEYS file against dev/mock-catalog's thirty apps.

A script is written by walking model.Sim alongside it: to say "down to that
row" the writer has to know which row the cursor is on, and the only honest
answer to that is the client's own view logic. So the plan and the prediction
come out of the same walk, and run.py simulates the finished file again from
scratch as a check that the two agree.

    python3 scenarios.py --seed 1 --run 7        # print one script
    python3 scenarios.py --seed 1 --runs 20      # the profile mix of a campaign
"""

import argparse
import random

from model import (TAB_KEY, TAB_ALL, TAB_UPDATES, TAB_BASKET, ROW_ACTION,
                   NOT_INSTALLED, CURRENT, UPDATE, Sim, load_world)

# ------------------------------------------------------------------ timing

# Two presses in one frame are ORed together by keys_pressed(), so every key
# needs a few frames of its own. A hand does not move faster than this either.
GAP_NAV = 140           # one step of the cursor or one tab along
GAP_ACT = 450           # a press that opens or answers something

# What the loop is away for. The mock packages are 85 KB, but every install
# is a fresh handshake, a download, a hash, an unpack and two renames, and a
# key that lands inside that window comes out the other side ORed with
# whatever else queued up. Measured on this rig at well under two seconds;
# the margin is deliberate and cheap.
INSTALL_MS = 2600
REMOVE_MS = 900
# SELECT -> fetch again: the whole catalog again, and keys_pressed() is not
# even consulted until it lands.
REFRESH_MS = 9000
# A scripted `shot` draws up to 360 frames waiting for the screen to settle.
SHOT_MS = 7000

# A script has to fit: 256 lines is keys_load()'s table, and a run wants to
# stay inside two minutes. It also has to be long enough to be worth starting
# an emulator for -- an errand that is over in six seconds says nothing about
# what the client does over a minute -- so a short one is padded out with
# somebody reading the list, which is what the rest of a session looks like.
MAX_KEYS = 240
MAX_MS = 45000
MIN_MS = 20000

# A row rested on is a screenshot fetched and a film started, on the media
# thread, behind the card. That is the load a hand actually puts on it, and
# it only happens when the cursor stops.
DWELL_MIN = 550
DWELL_MAX = 1800


class Planner:
    """A script being written, and the client's state as it is written."""

    def __init__(self, world, rng):
        self.sim = Sim(world)
        self.rng = rng
        self.t = 600                    # the first press, once the list is up
        self.script = []
        self.profiles = []

    # ------------------------------------------------------------ emit

    def room(self, keys=1, ms=0):
        return len(self.script) + keys <= MAX_KEYS and self.t + ms <= MAX_MS

    def key(self, name, gap=GAP_NAV):
        self.t += gap
        self.script.append((self.t, name))
        self.sim.press(name, self.t)

    def wait(self, ms):
        self.t += ms

    # -------------------------------------------------------- navigate

    def tab_at(self, tab):
        return self.sim.tabs.index(tab) if tab in self.sim.tabs else -1

    def goto_tab(self, tab):
        """L/R until the named tab is the active one, the short way round.
        False if that tab is not on screen at all -- the updates tab goes
        when the last update is taken, and the basket tab with the basket."""
        want = self.tab_at(tab)
        if want < 0:
            return False
        n = len(self.sim.tabs)
        if n <= 1 or want == self.sim.tab_at:
            return want == self.sim.tab_at
        forward = (want - self.sim.tab_at) % n
        back = n - forward
        # The triggers and the d-pad reach the tabs by the same line in
        # main.c; both are used, so both stay exercised.
        if forward <= back:
            name = self.rng.choice(("right", "rtrigger"))
            steps = forward
        else:
            name = self.rng.choice(("left", "ltrigger"))
            steps = back
        if not self.room(steps):
            return False
        for _ in range(steps):
            self.key(name)
        return True

    def goto_row(self, row):
        count = self.sim.view_count()
        if count <= 0 or row >= count:
            return False
        down = (row - self.sim.cursor) % count
        up = count - down
        steps, name = (down, "down") if down <= up else (up, "up")
        if not self.room(steps):
            return False
        for _ in range(steps):
            self.key(name)
        return self.sim.cursor == row

    def goto_app(self, index, prefer_category=False):
        """Onto the row that stands for one catalog entry, switching tabs if
        the entry is not under the one showing."""
        if prefer_category or self.sim.view_row(index) < 0:
            tab = TAB_KEY.index(self.sim.apps[index].category)
            if not self.goto_tab(tab):
                if self.sim.view_row(index) < 0:
                    return False
        row = self.sim.view_row(index)
        if row < 0:
            return False
        return self.goto_row(row)


# ---------------------------------------------------------------- profiles

def browser(p):
    """Scrolls a lot, opens nothing. The one that says whether the list
    itself costs anything."""
    for _ in range(p.rng.randint(20, 55)):
        if not p.room(1, GAP_NAV):
            return
        if p.rng.random() < 0.12 and len(p.sim.tabs) > 1:
            p.key(p.rng.choice(("left", "right", "ltrigger", "rtrigger")))
        else:
            p.key(p.rng.choices(("down", "up"), weights=(4, 1))[0])


def installer(p):
    """Three to five not-installed apps, one at a time: down to the row,
    cross for the question, cross for yes."""
    want = [a.index for a in p.sim.apps if a.state == NOT_INSTALLED]
    p.rng.shuffle(want)
    for index in want[:p.rng.randint(3, 5)]:
        if p.sim.apps[index].state != NOT_INSTALLED:
            continue
        if not p.room(8, GAP_ACT * 2 + INSTALL_MS):
            return
        if not p.goto_app(index, prefer_category=p.rng.random() < 0.5):
            continue
        p.key("cross", GAP_ACT)
        p.key("cross", GAP_ACT)
        p.wait(INSTALL_MS)


def updater(p):
    """The updates tab: the whole thing in one press, or two or three rows
    picked off by hand through the options menu."""
    if TAB_UPDATES not in p.sim.tabs:
        return browser(p)
    if p.rng.random() < 0.45:
        if not p.goto_tab(TAB_UPDATES) or not p.goto_row(0):
            return
        waiting = p.sim.action_plan()["apps"]
        if not p.room(2, GAP_ACT * 2 + waiting * INSTALL_MS):
            return
        p.key("cross", GAP_ACT)         # "Update N apps?"
        p.key("cross", GAP_ACT)
        p.wait(waiting * INSTALL_MS)
        return
    for _ in range(p.rng.randint(2, 3)):
        if TAB_UPDATES not in p.sim.tabs:
            return
        if not p.room(8, GAP_ACT * 2 + INSTALL_MS):
            return
        if not p.goto_tab(TAB_UPDATES):
            return
        rows = [r for r in range(p.sim.view_count()) if p.sim.view_index(r) >= 0]
        if not rows:
            return
        if not p.goto_row(p.rng.choice(rows)):
            return
        p.key("cross", GAP_ACT)         # the options menu, on "Update to X"
        p.key("cross", GAP_ACT)
        p.wait(INSTALL_MS)


def remover(p):
    """Two to four installed apps off the stick: square, then yes."""
    want = [a.index for a in p.sim.apps if a.state != NOT_INSTALLED]
    p.rng.shuffle(want)
    for index in want[:p.rng.randint(2, 4)]:
        if p.sim.apps[index].state == NOT_INSTALLED:
            continue
        if not p.room(8, GAP_ACT * 2 + REMOVE_MS):
            return
        if not p.goto_app(index, prefer_category=p.rng.random() < 0.5):
            continue
        p.key("square", GAP_ACT)
        p.key("cross", GAP_ACT)
        p.wait(REMOVE_MS)


def shopper(p):
    """Four to six rows set aside with triangle, gathered from the category
    tabs, then the basket tab and Download all."""
    want = list(range(len(p.sim.apps)))
    p.rng.shuffle(want)
    picked = 0
    for index in want:
        if picked >= p.rng.randint(4, 6):
            break
        if not p.room(10, GAP_NAV * 2):
            break
        if index in p.sim.basket:
            continue
        if not p.goto_app(index, prefer_category=True):
            continue
        p.key("triangle", GAP_NAV)
        picked += 1
    if TAB_BASKET not in p.sim.tabs:
        return
    if not p.goto_tab(TAB_BASKET) or not p.goto_row(0):
        return
    n = p.sim.action_plan()["apps"]
    if not p.room(2, GAP_ACT * 2 + n * INSTALL_MS):
        return
    p.key("cross", GAP_ACT)             # "Install N apps?"
    p.key("cross", GAP_ACT)
    p.wait(n * INSTALL_MS)


def reinstaller(p):
    """X on something already current: the options menu opens with Update
    greyed and the cursor on Reinstall."""
    want = [a.index for a in p.sim.apps if a.state == CURRENT]
    p.rng.shuffle(want)
    for index in want[:p.rng.randint(1, 2)]:
        if p.sim.apps[index].state != CURRENT:
            continue
        if not p.room(8, GAP_ACT * 2 + INSTALL_MS):
            return
        if not p.goto_app(index, prefer_category=p.rng.random() < 0.5):
            continue
        p.key("cross", GAP_ACT)
        p.key("cross", GAP_ACT)         # the cursor is already on Reinstall
        p.wait(INSTALL_MS)


def refresher(p):
    """SELECT, fetch again, and carry on browsing once it lands. The basket
    does not survive this and neither does a stale tab."""
    if not p.room(4, GAP_ACT * 2 + REFRESH_MS):
        return
    p.key("select", GAP_ACT)
    p.key("cross", GAP_ACT)             # the first info row is "fetch again"
    p.wait(REFRESH_MS)
    for _ in range(p.rng.randint(8, 18)):
        if not p.room(1, GAP_NAV):
            return
        p.key(p.rng.choices(("down", "up"), weights=(4, 1))[0])


def sweeper(p):
    """Held through the tabs, left and right, with a look down each."""
    for _ in range(p.rng.randint(10, 24)):
        if not p.room(4, GAP_NAV * 4):
            return
        p.key(p.rng.choice(("left", "right", "ltrigger", "rtrigger")))
        for _ in range(p.rng.randint(0, 3)):
            p.key("down")


def dwell(p, until_ms):
    """Somebody with the list in front of them, going down it at reading
    speed. What pads a short errand out to a session."""
    while p.t < until_ms and p.room(1, DWELL_MAX):
        gap = p.rng.randint(DWELL_MIN, DWELL_MAX)
        if p.rng.random() < 0.08 and len(p.sim.tabs) > 1:
            p.key(p.rng.choice(("left", "right")), gap)
        else:
            p.key(p.rng.choices(("down", "up"), weights=(5, 1))[0], gap)


PROFILES = [
    ("browser", browser, 4),
    ("installer", installer, 4),
    ("updater", updater, 3),
    ("remover", remover, 3),
    ("shopper", shopper, 3),
    ("reinstaller", reinstaller, 2),
    ("refresher", refresher, 2),
    ("sweeper", sweeper, 2),
]
MIX_WEIGHT = 4


def build(world, seed, run):
    """One run's script. The seed and the run number are the whole input, so
    a failure that happened once happens again."""
    rng = random.Random((seed << 20) ^ (run * 2654435761) ^ 0x5350)
    p = Planner(world, rng)

    names = [n for n, _f, _w in PROFILES] + ["mix"]
    weights = [w for _n, _f, w in PROFILES] + [MIX_WEIGHT]
    chosen = rng.choices(names, weights=weights)[0]
    by_name = {n: f for n, f, _w in PROFILES}

    if chosen == "mix":
        picks = rng.sample([n for n, _f, _w in PROFILES], rng.randint(2, 3))
        p.profiles = picks
        for name in picks:
            by_name[name](p)
    else:
        p.profiles = [chosen]
        by_name[chosen](p)

    # An errand that finished early becomes a session: the same person, still
    # looking, until the run is long enough to have said something.
    dwell(p, MIN_MS)

    # Every script ends the same way: a moment for the card to catch up, and
    # a settled photograph of wherever it all left the cursor.
    p.wait(1200)
    p.key("shot", GAP_ACT)
    p.sim.finish()
    return p


def render(script):
    return "".join("%d %s\n" % (at, key) for at, key in script)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--run", type=int)
    ap.add_argument("--runs", type=int)
    ap.add_argument("--site", default=None)
    args = ap.parse_args()

    world = load_world(args.site)
    if args.run is not None:
        p = build(world, args.seed, args.run)
        pred = p.sim.prediction()
        print("# run %d, %s: %d keys, %.1f s, %d installed after"
              % (args.run, "+".join(p.profiles), len(p.script),
                 p.t / 1000.0, len(pred["db"])))
        print(render(p.script), end="")
        return
    for run in range(1, (args.runs or 10) + 1):
        p = build(world, args.seed, run)
        pred = p.sim.prediction()
        print("%4d  %-24s %3d keys  %5.1f s  %2d installs  %2d removes  %2d records"
              % (run, "+".join(p.profiles), len(p.script), p.t / 1000.0,
                 pred["installs"],
                 sum(1 for e in pred["events"] if e[0] == "remove"),
                 len(pred["db"])))


if __name__ == "__main__":
    main()
