# soak

A hundred customers through the client, one emulator at a time, against
`dev/mock-catalog`'s thirty apps on the host. Each run is somebody with an
errand — install four things, take all the updates, fill a basket, or just
scroll — written as a `PSPDX.KEYS` file, played into the rig, and then held
against what the same errand was predicted to leave on the stick.

```sh
python3 app/tools/soak/run.py --runs 100 --seed 1    # the campaign
python3 app/tools/soak/run.py --perf 20              # twenty stress runs
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

## The four files

| | |
|---|---|
| `scenarios.py` | the customers: nine weighted profiles, seeded, so run 47 of seed 1 is always the same person |
| `model.py` | `main.c`'s input loop and `shell.c`'s view again in Python: what the cursor does, what the tabs do, and which records the run should leave |
| `run.py` | plants the state, writes the keys, runs the rig, reads the log while it is being written, and checks five things |
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
on this desk writes the same `PSPDX.LOG`. Each run stops the `pspdx-ppsspp`
unit and any emulator running `PSP/GAME/pspdx/EBOOT.PBP` — which is what
`dev/start` does, and for the same reason — and waits for anybody else's.
A run whose log turns out not to be its own is retried rather than reported.
The soak serves its catalog on port 8444 as `pspdx-soak-server`, so
`dev/start --mock` and its `pspdx-mock-server` on 8443 are left alone.
