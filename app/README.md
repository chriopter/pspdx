# pspdx-app

The on-device client. Right now it collects entropy, opens a TLS 1.3 connection
and prints the response — the groundwork under a package manager that does not
exist yet.

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
SDL_VIDEODRIVER=offscreen PPSSPPSDL "$PWD/EBOOT.PBP"
cat ~/.config/ppsspp/PSPDX.LOG
```

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
| `PSPDX.BMP` | the final screen, 480x272, 24-bit |
