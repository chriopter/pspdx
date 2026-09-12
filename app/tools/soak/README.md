# soak

A hundred customers through the client, one emulator at a time, against
`dev/mock-catalog`'s thirty apps on the host. Each run is somebody with an
errand — install four things, take all the updates, fill a basket, or just
scroll — written as a `PSPDX.KEYS` file, played into the rig, and then held
against what the same errand was predicted to leave on the stick.

```sh
python3 app/tools/soak/run.py --runs 100 --seed 1    # the campaign
python3 app/tools/soak/run.py --perf 20              # twenty stress runs
python3 app/tools/soak/run.py --edge 30              # the thirty edge cases
```

Both build the client against the loopback catalog first, serve that catalog,
and rebuild for the published one at the end. Exit status is non-zero if any
run failed. A run takes 60–130 seconds, so a hundred of them is about three
hours.

Before the first run, once, the wolfSSL prefix has to be where the Makefile
looks:

```sh
mkdir -p app/wolfssl-psp && cp -r ../pspdx/app/wolfssl-psp/prefix app/wolfssl-psp/
```

## The five files

| | |
|---|---|
| `scenarios.py` | the customers: eight weighted profiles and a mix of them, seeded, so run 47 of seed 1 is always the same person |
| `model.py` | `main.c`'s input loop and `shell.c`'s view again in Python: what the cursor does, what the tabs and the menu do, when the idle veil swallows a key, and which records the run should leave |
| `run.py` | plants the state, writes the keys, runs the rig, reads the log while it is being written, and checks five things |
| `edge.py` | the thirty edge cases: what to plant, which keys, and the oracle for each |
| `rig.sh` | the parts that want `tools/localcat/common.sh`: the throwaway CA, the two builds, the mock site and its server |

`python3 app/tools/soak/scenarios.py --seed 1 --run 7` prints one script and
what it is predicted to do, without starting anything.

## What is checked

Per run, and all five have to hold:

- **a — the log is this run's.** The first snapshot starts at `font:` then
  `ripple:`; the client started once; nothing in it belongs to the published
  catalog; the `keys: N scripted` count is the script's; and
  `catalog: 30 apps, 30 usable` is there, which is both "the fetch worked"
  and "the EBOOT was the one built for the host".
- **b — nothing failed and it was still running at the end.** No `failed`,
  `refusing`, `MISMATCH` or `cannot` anywhere, and the last `frames:` line is
  within fifteen seconds — of the *guest's* clock, which is the one that
  interval is counted in.
- **c — the stick is what the model said.** Every `PSP/PSPDX/db/<id>.json`
  under the mock's prefix, field by field — `id`, `rev`, `dir`, `manifest`,
  `version` — and every `PSP/GAME/` directory under it.
- **d — no late frames outside the installs.** Every ten-second window that
  had no install, removal, catalog fetch or screenshot in it must report
  `0 late`.
- **e — `PSPDX1.BMP` is there**, newer than the start of the run.

`--perf` checks the frame times instead and does not check the stick: the
held directions move the cursor out from under the key file on purpose, so
what gets installed is not knowable in advance. It reports, per run, the
worst frame, late frames per window, the worst `back` and `front` phase, the
worst audio callback and the free memory, and fails a run on any late frame
or any `frame N: X ms` line outside an install or a screenshot.

## What a failure looks like

```
  47  installer+remover            51 keys  91s   4 inst  2 rm  worst  1701 ms  late  9  FAIL
        c: dev.pspdx.mock.09.thirteentricks: rev is 1788999500, expected 1789009000
        d: window 3 (48-64 s in) had 2 late frames (worst 42 ms) with no install in it
```

and then, at the end:

```
100 runs, 1 failed, 9210 s of emulator
  1-47: c: dev.pspdx.mock.09.thirteentricks: rev is 1788999500, expected 1789009000
         kept under app/tools/soak/results/1-47
```

`results/<seed>-<run>.json` is written for every run, pass or fail. A failed
run also leaves `results/<seed>-<run>/` with the key script it was given, the
stitched log with a host timestamp on every line, and the screenshots. Replay
it on its own with

```sh
python3 app/tools/soak/run.py --runs 1 --seed 1 --from 47
```

## Edge cases

```sh
python3 app/tools/soak/run.py --edge 30
python3 app/tools/soak/run.py --edge 1 --only zip-deep-eboot
```

The soak says what the client does when nothing goes wrong. `edge.py` is the
other half: thirty named scenarios, each one a thing that can go wrong, each
with its own oracle. They are deterministic — no run is drawn at random — and
they take about eighty minutes together.

A scenario is a function returning what to plant, which keys to play, how
long to leave the emulator up, and what has to be true afterwards. Where the
client cannot do the damage itself the harness does it: `tools/localcat/serve.py`
reads a `faults.json` beside the site directory before every request and will
answer 404 or 500, cut a body short under a `Content-Length` that promised
more, or go quiet for twenty seconds in the middle of one; the mock server is
stopped and started under a running emulator from a per-scenario timeline;
and a storm of four hundred presses at fifty milliseconds goes through
PPSSPP's debugger, because `PSPDX.KEYS` holds 256 lines in four kilobytes.

| | what it does |
|---|---|
| `storm-mixed-keys` | 20 s of seven buttons 40–60 ms apart, through the debugger (not X or triangle: on an installed row either opens the menu on Run, and the next X is Run) |
| `storm-confirm-band` | the install question opened and cancelled thirty times |
| `storm-bands` | the options menu and the info band, thirty open-and-close each |
| `storm-refresh-five` | five catalog refetches back to back |
| `storm-refresh-basket` | a refetch with five in the basket |
| `bulk-basket-thirty` | all thirty in the basket, Download all |
| `bulk-update-all` | every update waiting, in one press |
| `bulk-remove-twenty` | all twenty installed removed one at a time |
| `bulk-install-remove-install` | fifteen in, fifteen out, fifteen in again (a removal is five keys through the menu, and the key file holds 256 lines) |
| `net-server-killed` | the server stopped mid-run and started again |
| `net-404-and-500` | a release that answers 404 and one that answers 500 |
| `net-truncated-body` | a body cut short under its own Content-Length |
| `net-wrong-sha256` | bytes that do not hash to what the catalog said |
| `net-stall-then-resume` | a download quiet for 22 s, under the 30 s timeout |
| `net-offline-at-start` | nothing listening at boot, X to retry, then the catalog |
| `net-catalog-truncated` | the catalog itself cut off mid-body |
| `net-slow-link` | 60 KB/s and a hand on the pad throughout |
| `zip-no-eboot-and-two-eboots` | no EBOOT.PBP, and two at the same depth |
| `zip-path-escapes` | entries called `../../evil` and `/PSP/GAME/evil` |
| `zip-header-lies` | central directory against local header, and a body that is not a zip |
| `zip-stored-and-empty-file` | method 0, a zero-byte file, a subdirectory |
| `zip-two-thousand-files` | 2000 tiny files in one archive |
| `zip-deep-eboot` | the EBOOT four directories down |
| `catalog-bad-entries` | six unusable entries among the thirty |
| `catalog-hostile-strings` | a 200-character name, a 300-character summary, a 40-character version, a duplicated id, three assets that 404 |
| `catalog-seventy-apps` | seventy entries against `MAX_APPS` 64 |
| `stick-broken-records` | garbage, half a JSON document, a record with no directory, a directory with no record |
| `stick-interrupted-install` | a leftover staging tree and two `.old` directories |
| `stick-orphan-and-self-record` | a record for an id the catalog does not have, and a self record with the wrong version |
| `idle-six-minutes` | six minutes of browsing at reading speed, films looping |

Every scenario is judged on its own oracle *and* on five things it does not
have to ask for:

- the log is this run's, and the client started once;
- the loop was still drawing at the end — a `frames:` line after the shot and
  within reach of when the emulator was stopped;
- the script's last two keys, a `down` and a `shot`, reached it, and
  `PSPDX1.BMP` is on the stick: the client is still usable after whatever was
  done to it;
- no line says `failed`, `refusing`, `MISMATCH` or `cannot` unless the
  scenario named what it broke;
- the stick is whole: every record has the directory it names, every
  `PSPDXMock*` directory has a record, and there is no `.pspdx-stage` or
  `<dir>.old` left behind.

`results/edge-<name>.json` is written for every scenario; a failed one also
leaves `results/edge-<name>/` with its keys, its stitched log and its
screenshots. The table at the end lists all thirty with their worst frame and
their verdict.

The first full campaign found one thing, and `storm-mixed-keys` found it:
SELECT, down, X was then the info band's second row, "sweep the field again", and
`entropy_screen_run()` only ended when the pool was full *and* X was pressed.
With no hand on the analog stick the bar never moves, so two presses from the
list put the console in a screen with no way out -- the main loop stopped,
the log stopped with it, and nothing the key file pressed afterwards was ever
read, because the sweep reads the pad itself. O now leaves that sweep and the
pool it was replacing is put back (`entropy_stash` / `entropy_restore`), so
walking out of it costs the session nothing. The first sweep of a run, which
has no pool behind it, is not offered the way out and still has to be
finished.

## What the model knows about the keys

The model is the oracle, so it mirrors `main.c` as it is now and not as the
first campaign knew it:

- **Tabs.** The gear tab is leftmost and always there; walking onto it opens
  the info band and walking off it closes it, and inside the band O or X
  steps one tab to the right. The stick tab, present while anything is
  installed, lists what is installed with the updates first; its action row
  "Update all" is only there while an update waits, and takes the updates
  alone. The basket tab is as it was. The tabs walk under the info band but
  not under a question, the menu or the details band. A tab that vanished
  falls back to All.
- **Keys on a row.** X asks to install or update a row that is not current
  and opens the options menu on one that is; triangle opens the menu; square
  toggles the basket; SELECT does nothing. The menu is Run, Reinstall,
  Delete, basket, Information, the cursor starting on Run for anything
  installed and on the basket row otherwise, greyed rows stepped over.
- **What a script may not press.** Run, whether by START or by X on the
  menu's first row, and the band's two typing rows, which hand the pad to
  the firmware keyboard, and its sweep, which reads the pad itself. The model
  raises on all of them; a script that reached one would not be a script.
- **The idle veil.** Ten seconds without a key and the shell fades; the first
  key after that only lifts the veil. The model swallows a key that comes
  more than ten seconds after the previous one with nothing open. After an
  install or a refetch the loop was away for a length of time only the
  emulator knows, so the planners never let that decide anything: a key
  that would come more than nine seconds after the last one is preceded by a
  circle, which does nothing on the browser whether the veil was down or
  not.
- **The info band's rows** are Update catalog, Add a list or repository,
  Install from GitHub, and the sweep. The refresher reaches the first by
  walking left to the gear tab and pressing X; the other three are never
  pressed.

## Two things about this desk

**The log is a ring of forty lines.** `util/runtime.c` keeps the last forty
and rewrites `PSPDX.LOG` whole every ten seconds and after every install. A
minute-long run overruns that several times, so the file at the end is not
the run. `run.py` therefore reads the file while the emulator is up, several
times a second, and stitches the snapshots by their overlap. The `frames:`
lines are never lost — the dump follows them in the same breath — and neither
is anything an install writes; a quiet stretch of picture fetches between two
dumps can be, and `results/<seed>-<run>.json` says how often that happened as
`log_gaps`.

**The guest does not keep up.** PPSSPP gets through about six of the PSP's
seconds for every ten of the host's on this workload, so a key file timed in
guest milliseconds takes about 1.6 times as long on a wall clock. Every run
measures its own factor — the scripted `shot` is the last key, and its
screenshot's mtime dates it — and reports it as `slowdown`. A run that ended
before its own last key is thrown away and started again with a longer
estimate.

**One emulator at a time.** The memory stick is one directory and everything
on this desk writes the same `PSPDX.LOG`. A run waits until no emulator is
up at all and ends nothing it did not start: a session somebody opened by
hand is theirs to close, and the rig says it is waiting rather than taking
the desk. Each run's own emulator is stopped by its own `run-ppsspp.sh` when
its seconds are up.
A run whose log turns out not to be its own is retried rather than reported.
The soak serves its catalog on port 8444 as `pspdx-soak-server`, so
`dev/start --mock` and its `pspdx-mock-server` on 8443 are left alone.
