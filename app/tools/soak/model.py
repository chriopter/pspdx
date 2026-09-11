#!/usr/bin/env python3
"""What the client would do, worked out on a desk.

A key script is a list of button presses at known times. The client's input
loop -- app/main.c, with the view and the tabs from app/gui/shell.c -- turns
that into a cursor walking a filtered list, questions answered, packages
installed and removed. This file is that loop again in Python, close enough
that the set of records under PSP/PSPDX/db and the directories under PSP/GAME
can be predicted before the emulator is started, and then held against what
the run actually left.

It is a model of the *decisions*, not of the machine: it assumes every
install and every fetch succeeds, because a failure is what the harness is
looking for and a model that predicted failures would have nothing to catch.

Where main.c and shell.c are mirrored line for line the comment says so.
Where the code left a choice open, the comment says THE CODE IS AMBIGUOUS and
run.py's report repeats it.
"""

import importlib.machinery
import importlib.util
import json
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# ----------------------------------------------------------------- buttons

# Names as app/main.c's button_named() spells them. START is deliberately
# absent from every script this harness writes: launch_app() hands the PSP to
# the package with sceKernelLoadExec and the run is over, log and all.
KEYS = ("up", "down", "left", "right", "cross", "circle", "square",
        "triangle", "select", "ltrigger", "rtrigger", "shot")

# -------------------------------------------------------------------- tabs

# shell.c: TAB_NAME / TAB_KEY, and the two tabs that are not categories.
TAB_KEY = ("", "games", "demos", "apps", "emulators", "plugins")
TAB_ALL = 6
TAB_UPDATES = -2
TAB_BASKET = -1
ROW_ACTION = -2

NOT_INSTALLED, CURRENT, UPDATE = "none", "current", "update"

# main.c: enum choice
CHOICE_UPDATE, CHOICE_REINSTALL, CHOICE_DELETE = 0, 1, 2

# --------------------------------------------------------------- the world

class App:
    """One catalog entry, plus what the stick says about it."""

    def __init__(self, index, id, name, category, rev, version, size, dirname):
        self.index = index
        self.id = id
        self.name = name
        self.category = category
        self.rev = rev                  # release.rev in the catalog
        self.version = version          # release.version in the catalog
        self.size = size
        self.dir = dirname              # the directory its archive unpacks to
        self.has_release = bool(rev and size)
        self.state = NOT_INSTALLED
        self.local_rev = 0
        self.local_version = ""


def load_mock_module():
    """dev/mock-catalog, imported rather than re-implemented: the ids, the
    directory names and the seeded thirds are its business and copying them
    here would be a second source of truth to keep in step."""
    path = os.path.join(REPO, "dev", "mock-catalog")
    loader = importlib.machinery.SourceFileLoader("pspdx_mock_catalog", path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def mock_plan():
    """The state mock-catalog plants for each row, recomputed from its own
    seed. make() draws the thirds before it draws anything else, so the same
    Random in the same order gives the same list."""
    mc = load_mock_module()
    rng = random.Random(mc.SEED)
    third = mc.COUNT // 3
    states = (["current"] * third + ["update"] * third +
              ["none"] * (mc.COUNT - 2 * third))
    rng.shuffle(states)
    rows = []
    for i, (name, category, _author, _kinds) in enumerate(mc.ROWS[:mc.COUNT]):
        rows.append({
            "index": i,
            "id": "%s%02d.%s" % (mc.ID_PREFIX, i, mc.slug(name).lower()),
            "dir": "%s%02d%s" % (mc.DIR_PREFIX, i, mc.slug(name)),
            "name": name,
            "category": category,
            "rev": mc.REV_BASE + i * 1000,
            "version": "%d.%d.%d" % (1 + i // 10, i % 10, i % 3),
            "old_rev": mc.REV_BASE + i * 1000 - 500,
            "old_version": "%d.%d.%d" % (i // 10, i % 10, i % 3),
            "state": states[i],
        })
    return mc, rows


def load_world(site_dir=None):
    """The catalog as the client will parse it, and the stick as mock-catalog
    will have planted it. The sizes come from the generated site when it is
    there, so the confirm band's tally is the real one; without it the model
    only needs to know a size is not zero."""
    mc, rows = mock_plan()
    sizes = {}
    if site_dir:
        path = os.path.join(site_dir, "catalog.json")
        if os.path.exists(path):
            for entry in json.load(open(path))["apps"]:
                sizes[entry["id"]] = entry["release"]["size"]

    apps = []
    db = {}
    dirs = set()
    for row in rows:
        app = App(row["index"], row["id"], row["name"], row["category"],
                  row["rev"], row["version"], sizes.get(row["id"], 1), row["dir"])
        if row["state"] == "current":
            app.local_rev, app.local_version = row["rev"], row["version"]
        elif row["state"] == "update":
            app.local_rev, app.local_version = row["old_rev"], row["old_version"]
        if row["state"] != "none":
            # catalog.c's parse() marks it APP_UNKNOWN and check_updates()
            # then compares release.rev against the record's.
            app.state = UPDATE if app.rev > app.local_rev else CURRENT
            db[app.id] = {"id": app.id, "rev": app.local_rev, "dir": app.dir,
                          "manifest": "", "version": app.local_version}
            dirs.add(app.dir)
        apps.append(app)
    return {"apps": apps, "db": db, "dirs": dirs,
            "id_prefix": mc.ID_PREFIX, "dir_prefix": mc.DIR_PREFIX}


# ------------------------------------------------------------- the machine

class Sim:
    """app/main.c's loop and app/gui/shell.c's view, as far as a script can
    reach them. One press per call: the harness never puts two keys on one
    millisecond, because keys_pressed() ORs everything whose moment has
    passed into a single frame's `pressed` and two buttons in one frame is
    not a thing a hand does."""

    # How long the client is away from the input loop for one install. Only
    # the script writer uses this; the model itself has no clock.
    def __init__(self, world):
        self.apps = [App(a.index, a.id, a.name, a.category, a.rev, a.version,
                         a.size, a.dir) for a in world["apps"]]
        for mine, theirs in zip(self.apps, world["apps"]):
            mine.state = theirs.state
            mine.local_rev = theirs.local_rev
            mine.local_version = theirs.local_version
        self.db = {k: dict(v) for k, v in world["db"].items()}
        self.dirs = set(world["dirs"])

        self.basket = set()
        self.tabs = []
        self.tab_at = 0
        self.view = []
        self.view_action = 0
        self.cursor = 0

        self.question = None            # None / "install" / "remove" / "all"
        self.question_of = -1
        self.menu_open = False
        self.menu_cursor = 0
        self.menu_of = -1
        self.menu_on = [0, 0, 0]
        self.info = False
        self.info_action = 0

        self.installs = 0               # how many install_app() calls happened
        self.events = []                # ("install"|"remove"|"refresh", t, id)
        self.shots = 0
        self.refresh_pending_at = None
        self.keep = ""

        # main.c after sync_done(): cursor 0, then the view built afresh.
        self.cursor = 0
        self.view_rebuild()

    # ---------------------------------------------------------- shell.c

    def updates_waiting(self):
        return sum(1 for a in self.apps if a.state == UPDATE)

    def collect_tabs(self, keep):
        found = False
        self.tabs = []
        self.tab_at = 0
        if not self.apps:
            return False
        if self.updates_waiting() > 0:
            self.tabs.append(TAB_UPDATES)
        if self.basket:
            self.tabs.append(TAB_BASKET)
        for t in range(TAB_ALL):
            has = not TAB_KEY[t]
            if not has:
                has = any(a.category == TAB_KEY[t] for a in self.apps)
            if has:
                self.tabs.append(t)
        for i, tab in enumerate(self.tabs):
            if tab == keep:
                self.tab_at = i
                found = True
        return found

    def build_view(self):
        tab = self.tabs[self.tab_at] if self.tabs else 0
        self.view = []
        self.view_action = 0
        if not self.apps or not self.tabs:
            return
        for i, a in enumerate(self.apps):
            if tab == TAB_UPDATES:
                take = a.state == UPDATE
            elif tab == TAB_BASKET:
                take = i in self.basket
            else:
                take = (not TAB_KEY[tab]) or a.category == TAB_KEY[tab]
            if take:
                self.view.append(i)
        self.view_action = 1 if tab < 0 else 0

    def view_rebuild(self):
        was = self.tabs[self.tab_at] if self.tabs else 0
        self.basket.clear()             # shell_view_rebuild() drops it whole
        self.collect_tabs(was)
        self.build_view()

    def tabs_refresh(self):
        was = self.tabs[self.tab_at] if self.tabs else 0
        kept = self.collect_tabs(was)
        self.build_view()
        return kept

    def view_count(self):
        return len(self.view) + self.view_action

    def view_index(self, row):
        if self.view_action and row == 0:
            return ROW_ACTION
        row -= self.view_action
        return self.view[row] if 0 <= row < len(self.view) else -1

    def view_row(self, index):
        for row, at in enumerate(self.view):
            if at == index:
                return row + self.view_action
        return -1

    def tab_kind(self):
        tab = self.tabs[self.tab_at] if self.tabs else 0
        return tab

    def action_plan(self):
        plan = {"apps": 0, "again": 0, "skipped": 0, "bytes": 0, "updates": 0}
        if not self.view_action:
            return plan
        plan["updates"] = 1 if self.tabs[self.tab_at] == TAB_UPDATES else 0
        for at in self.view:
            a = self.apps[at]
            if not a.has_release or not a.size:
                plan["skipped"] += 1
                continue
            plan["apps"] += 1
            plan["bytes"] += a.size
            if a.state == CURRENT:
                plan["again"] += 1
        return plan

    def tab_move(self, step):
        if len(self.tabs) <= 1:
            return
        self.tab_at = (self.tab_at + step + len(self.tabs)) % len(self.tabs)
        self.build_view()

    # ----------------------------------------------------------- main.c

    def view_settled(self):
        at = self.view_index(self.cursor)
        if not self.tabs_refresh():
            self.cursor = 0
            return
        row = self.view_row(at) if at >= 0 else -1
        count = self.view_count()
        if row >= 0:
            self.cursor = row
        elif self.cursor >= count:
            self.cursor = count - 1 if count > 0 else 0

    def install_app(self, index, t):
        """install_app() with rc 0. A failure is what the rig is for."""
        a = self.apps[index]
        a.state = CURRENT
        a.local_rev = a.rev
        a.local_version = a.version
        # install.c db_write(): the release out of the catalog carries no
        # manifest URL, so the record's "manifest" is empty.
        self.db[a.id] = {"id": a.id, "rev": a.rev, "dir": a.dir,
                         "manifest": "", "version": a.version}
        self.dirs.add(a.dir)
        self.installs += 1
        self.events.append(("install", t, a.id))

    def uninstall_app(self, index, t):
        a = self.apps[index]
        a.state = NOT_INSTALLED
        a.local_rev = 0
        a.local_version = ""
        self.db.pop(a.id, None)
        self.dirs.discard(a.dir)
        self.events.append(("remove", t, a.id))

    def install_all(self, t):
        """main.c install_all(): the rows are read into a list before the
        first fetch, because an install moves the entry out of the updates
        view under a loop still walking it."""
        picked = []
        for row in range(self.view_count()):
            at = self.view_index(row)
            if at < 0:
                continue
            a = self.apps[at]
            if not a.has_release or not a.size:
                continue
            picked.append(at)
        for at in picked:
            self.install_app(at, t)
            self.basket.discard(at)     # shell_basket_forget on success
        return len(picked)

    def ask_install(self, index):
        self.question = "install"
        self.question_of = index

    def ask_remove(self, index):
        a = self.apps[index]
        record = self.db.get(a.id)
        if not record or not record["dir"]:
            return                      # shell_status, and no question
        self.question = "remove"
        self.question_of = index

    def ask_all(self):
        if self.action_plan()["apps"] <= 0:
            return
        self.question = "all"
        self.question_of = -1

    def menu_open_for(self, index):
        a = self.apps[index]
        update = a.state == UPDATE
        self.menu_on = [1 if update else 0, 0 if update else 1, 1]
        self.menu_cursor = CHOICE_UPDATE if update else CHOICE_REINSTALL
        self.menu_of = index
        self.menu_open = True

    def menu_move(self, by):
        for _ in range(3):
            self.menu_cursor = (self.menu_cursor + by + 3) % 3
            if self.menu_on[self.menu_cursor]:
                break

    def refresh_start(self, t):
        """SELECT -> fetch again. sync_start() is called and `synced` goes to
        zero, which stops keys_pressed() from being consulted at all: every
        scripted key whose moment passes while the catalog is being fetched
        is ORed into one frame when it comes back. The scripts this harness
        writes leave that window empty; press() refuses one that does not."""
        at = self.view_index(self.cursor)
        self.keep = self.apps[at].id if at >= 0 else ""
        self.refresh_pending_at = t
        self.events.append(("refresh", t, self.keep))

    def refresh_finish(self):
        """The second sync coming back: the catalog is parsed again from what
        is now on the stick, the view is rebuilt (which drops the basket) and
        the cursor goes back to the package it was on."""
        for a in self.apps:
            record = self.db.get(a.id)
            if record:
                a.local_rev = record["rev"]
                a.local_version = record["version"]
                a.state = UPDATE if a.rev > a.local_rev else CURRENT
            else:
                a.state = NOT_INSTALLED
                a.local_rev = 0
                a.local_version = ""
        self.cursor = 0
        self.view_rebuild()
        if self.keep:
            for a in self.apps:
                if a.id == self.keep:
                    row = self.view_row(a.index)
                    if row >= 0:
                        self.cursor = row
                    break
            self.keep = ""
        self.refresh_pending_at = None

    # ------------------------------------------------------------ input

    def press(self, key, t=0, refresh_ms=8000):
        if self.refresh_pending_at is not None:
            if t - self.refresh_pending_at < refresh_ms:
                raise ValueError(
                    "key %r at %d falls inside the refetch window that started "
                    "at %d; the client would queue it and fire it with the "
                    "others" % (key, t, self.refresh_pending_at))
            self.refresh_finish()

        count = self.view_count() if self.apps else 0

        # The triggers and left/right walk the tabs. main.c does this above
        # the modal test, so a question standing on screen does not stop
        # them -- see the report: THE CODE IS AMBIGUOUS about whether that
        # is meant. No script here presses them while something is open.
        if key in ("ltrigger", "rtrigger", "left", "right") and count > 0:
            self.tab_move(1 if key in ("rtrigger", "right") else -1)
            self.cursor = 0
            count = self.view_count()

        modal = self.question is not None or self.menu_open or self.info
        if key == "down" and count > 0 and not modal:
            self.cursor = (self.cursor + 1) % count
        if key == "up" and count > 0 and not modal:
            self.cursor = (self.cursor + count - 1) % count
        if key == "shot":
            self.shots += 1

        if self.question is not None:
            if key == "cross":
                asked, index = self.question, self.question_of
                self.question = None
                if asked == "install":
                    self.install_app(index, t)
                elif asked == "all":
                    self.install_all(t)
                else:
                    self.uninstall_app(index, t)
                self.view_settled()
            elif key == "circle":
                self.question = None
        elif self.menu_open:
            if key == "down":
                self.menu_move(1)
            if key == "up":
                self.menu_move(-1)
            if key == "circle":
                self.menu_open = False
            elif key == "cross":
                chosen, index = self.menu_cursor, self.menu_of
                self.menu_open = False
                if chosen == CHOICE_DELETE:
                    self.ask_remove(index)
                else:
                    self.install_app(index, t)
                    self.view_settled()
        elif self.info:
            if key == "down":
                self.info_action = (self.info_action + 1) % 2
            if key == "up":
                self.info_action = (self.info_action + 1) % 2
            if key in ("select", "circle"):
                self.info = False
            elif key == "cross":
                self.info = False
                if self.info_action == 0:
                    self.refresh_start(t)
                # action 1 sweeps the entropy field again: it changes
                # nothing this model predicts, and the scripts avoid it
                # because the sweep takes the loop away for seconds.
        elif key == "select":
            self.info = True
        elif count > 0:
            at = self.view_index(self.cursor)
            if key == "cross":
                if at == ROW_ACTION:
                    self.ask_all()
                elif at >= 0 and self.apps[at].state == NOT_INSTALLED:
                    self.ask_install(at)
                elif at >= 0:
                    self.menu_open_for(at)
            if key == "triangle" and at >= 0:
                if at in self.basket:
                    self.basket.discard(at)
                else:
                    self.basket.add(at)
                self.view_settled()
            if key == "square" and at >= 0 and self.apps[at].state != NOT_INSTALLED:
                self.ask_remove(at)
            # START is never scripted: it would hand the PSP to the package.

    # ----------------------------------------------------------- result

    def finish(self, refresh_ms=8000):
        """Anything still in flight when the keys ran out."""
        if self.refresh_pending_at is not None:
            self.refresh_finish()

    def prediction(self):
        return {
            "db": {k: dict(v) for k, v in sorted(self.db.items())},
            "dirs": sorted(self.dirs),
            "installs": self.installs,
            "shots": self.shots,
            "events": list(self.events),
        }


def parse_script(text):
    """A PSPDX.KEYS file back into (ms, key) pairs, the way keys_load() reads
    it: the first two whitespace-separated fields of every line that has
    them, in file order."""
    out = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        out.append((int(parts[0]), parts[1]))
    return out


def simulate(world, script):
    """A key script against a freshly planted stick. Returns the prediction."""
    sim = Sim(world)
    for at, key in script:
        sim.press(key, at)
    sim.finish()
    return sim.prediction()
