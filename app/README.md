# pspdx-app

The on-device client. It collects entropy, fetches the catalog over TLS 1.3,
and installs or updates a package from its manifest.

## How it is laid out

One directory per layer; every include names its directory, so the layer a
header comes from is visible at the include line.

| Directory | What |
|---|---|
| `main.c` | the controller: input loop, screens, nothing else |
| `gui/` | drawing on the debug screen; owns the 60-column layout width |
| `logic/` | the entropy pool |
| `update/` | the catalog and what is out of date |
| `install/` | manifest, download, verify, unpack, the on-stick database |
| `network/` | HTTPS and the compiled-in roots |
| `util/` | logging and the millisecond clock |

Dependencies run one way: `gui` → `update` → `install` → `network`, with
`util` under all of them.

## What it trusts

The chain is verified against `network/ca_certs.h`, 17 roots compiled in. The
PSP has no CA store worth using, so the client carries its own; regenerate it with

```sh
python3 tools/make-ca-bundle.py
```

A host whose CA is not in there fails the handshake and names the CA in
`PSPDX.LOG`. That is the signal to add it.

## Building

```sh
docker run --rm -v "$PWD:/src" -w /src pspdev/pspdev:latest sh wolfssl-psp/build.sh
docker run --rm -v "$PWD:/src" -w /src pspdev/pspdev:latest make
```

The first line builds wolfSSL into `wolfssl-psp/prefix` (a few minutes, once);
the second produces `EBOOT.PBP`. Copy it to `ms0:/PSP/GAME/pspdx/`, or open it
in PPSSPP.

Without Docker, the same works with a pspdev release tarball unpacked
anywhere: set `PSPDEV` to it and put its `bin/` on `PATH`. CMake is needed for
the wolfSSL step.

## Running it without a screen

The app writes what it did to the memory stick, so a run needs no window:

```sh
sh run-ppsspp.sh 300     # seconds to let it run
```

It copies the EBOOT to the emulator's memory stick, replays the recorded
sweep, and leaves `PSPDX.LOG` and `shot.png` beside itself.

PPSSPP only flushes an emulated file to the host on close, which is why the log
is written in one go at the end rather than line by line.

The final screen is also dumped to `PSPDX.BMP`, straight from the framebuffer.
That is the only screenshot path that works on a real PSP, and on a host whose
desktop is locked.

## Files it leaves on the stick

| Path | What |
|---|---|
| `PSPDX.SEED` | 20 bytes of pool state, so the entropy ritual happens once |
| `PSPDX.TRACE` | every pad sample of the last sweep, for replay |
| `PSPDX.REPLAY` | if present, the sweep replays `PSPDX.TRACE` instead of reading the stick |
| `PSPDX.LOG` | what the run did |
| `PSPDX.HTTP` | the raw response of the last fetch |
| `PSPDX.BMP` | the catalog screen, 480x272, 24-bit |
| `PSPDX2.BMP` | the screen after an install |
| `PSPDX.INSTALL` | a manifest URL here installs that app unattended, for testing |
| `PSP/PSPDX/db/<id>.json` | what was installed: rev, directory, manifest URL |
