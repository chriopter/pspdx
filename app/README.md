# pspdx-app

The on-device client. It collects entropy, fetches the catalog over TLS 1.3,
and installs or updates a package from its manifest.

## How it is laid out

One directory per layer; every include names its directory, so the layer a
header comes from is visible at the include line.

| Directory | What |
|---|---|
| `main.c` | the controller: input loop, screens, nothing else |
| `gui/` | the browser on the GE: `shell` lays out, `lattice` is the moving backdrop, `preview` owns what is on the card, `gfx` draws, `font` is the PSP's own face; the entropy sweep floods the same surface and leaves it behind as the backdrop |
| `video/` | the film on the card: `mp4` finds the H.264 in the catalog's clip, `psmf` wraps it the way the PSP's decoder wants it, `player` runs that decoder; only `player.c` knows it is on a PSP |
| `audio/` | a piano and a glass, the tune, and the sounds the interface makes; only `audio.c` knows it is on a PSP |
| `logic/` | the entropy pool |
| `update/` | the catalog, what is out of date, and the pictures it links to |
| `install/` | manifest, download, verify, unpack, the on-stick database |
| `network/` | HTTPS and the compiled-in roots |
| `util/` | logging and the millisecond clock |

Dependencies run one way: `gui` → `update` → `install` → `network`, with
`util` under all of them. Nothing in `update/`, `install/` or `network/`
draws or plays a note: `gui/preview.c` asks `update/assets.c` for bytes and
turns them into a texture itself, and the shell posts sounds into a queue
that the audio thread empties. The code that has to be right and the code
that has to look good do not share a file.

The card the picture sits on is the one thing drawn in real perspective; it
drifts and leans a little, and a sweep of light crosses it now and then.
Once the still is up, the entry's film fades in over it and loops.

## The film

The catalog serves a plain MP4 -- H.264 baseline, 480x272 at 30 -- the same
file a PSP plays out of `PSP/VIDEO`. The PSP's decoder, `sceMpeg` on the
Media Engine, does not read MP4 though: it wants the same H.264 in a PSMF,
Sony's envelope -- a header, then an MPEG-2 program stream in 2048-byte
packs. So the client wraps the film itself, in RAM, on the way in:
`video/mp4.c` finds the samples and the parameter sets, `video/psmf.c`
packs them, `video/player.c` feeds the result through a ring buffer and
decodes straight into the card's texture, thirty pictures a second by the
clock. Nothing Sony-shaped ever touches the catalog or the network.

The wrapping runs on a desk too, where ffmpeg can check it:

```sh
cc -I. video/mp4.c video/psmf.c tools/mp4-to-psmf.c
./a.out video.mp4 video.psmf && ffprobe video.psmf
```

Two things the decoder does that the code has to know. It writes every
pixel with alpha zero, so the film's texture is drawn as colour only. And
on PPSSPP it takes a little longer than real time per picture, which is why
the film is paced by the clock rather than by the frame: a slow frame is
followed by two pictures, not by drift.

The PSMF header carries what PPSSPP's own parser reads from real files; a
PSP has not run it yet.

The tune renders on a desk with the same code the PSP runs, which is how it
gets listened to:

```sh
cc -I. audio/synth.c audio/music.c audio/cues.c tools/render-music.c -lm
./a.out music.wav
```

## The TLS client

wolfSSL over `sceNetInet` sockets, TLS 1.3 only. It offers X25519 ahead of
P-256 and ChaCha20-Poly1305 ahead of AES-128-GCM, because this CPU has no AES
instruction; `network/bench.c` measures the gap on the device itself. The key
share rides along with the ClientHello, so no HelloRetryRequest and one
handshake covers the whole catalog.

Its randomness is the sweep: one bit for every newly touched point of an
invisible 250x250 field, until the pool holds the 128 that X25519 and
ChaCha20-Poly1305 stand on. The pool goes on taking packet arrival times and
battery readings for the rest of the run, uncounted, and reaches `PSPDX.SEED`
once, on the way out through HOME.

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

## Running it on a desk

```sh
dev/start           # build, then open it in PPSSPP, one instance, straight to the catalog
dev/start-reset     # the same without the seed: the sweep runs, as on a new PSP
```

Both plant what a run needs on the emulator's stick -- the system font,
the clips from the catalog repo next door, a seed -- and stop whatever
instance was running before, since two at once write the same files and
play the same tune slightly apart.

## Running it without a screen

The app writes what it did to the memory stick, so a run needs no window:

```sh
sh run-ppsspp.sh            # 25 seconds, straight to the catalog
sh run-ppsspp.sh 90 --sweep # replay the recorded sweep first
```

It copies the EBOOT to the emulator's memory stick and leaves `PSPDX.LOG` and
`shot.png` beside itself. By default it also plants a fixed seed, so the
client skips the sweep the way it does on a PSP after its first run and is
at the catalog a few seconds in; `--sweep` replays `testdata/sweep.trace`
through the entropy screen instead. A replayed sweep never writes a seed,
because replayed input is not entropy.

PPSSPP does not mount `flash0:` for the guest, so the script also copies the
emulator's own copy of the system font to where the client's fallback path
looks. On hardware the font comes out of the firmware.

PPSSPP only flushes an emulated file to the host on close, which is why the log
is written in one go at the end rather than line by line.

The final screen is also dumped to `PSPDX.BMP`. It is read back through the
GE rather than straight out of VRAM: on PPSSPP the memory behind a GE-drawn
frame is not kept current, and a CPU read hands back a frame that is long
gone. Copying through the GE is what games do for their save icons, and it
is the path the emulator keeps honest. That is also the only screenshot path
that works on a real PSP, and on a host whose desktop is locked.

## Files it leaves on the stick

| Path | What |
|---|---|
| `PSPDX.SEED` | 20 bytes of pool state, so the sweep happens once; rewritten on the way out through HOME |
| `PSPDX.TRACE` | every pad sample of the last sweep, for replay |
| `PSPDX.REPLAY` | if present, the sweep replays `PSPDX.TRACE` instead of reading the stick |
| `PSPDX.LOG` | what the run did |
| `PSPDX.HTTP` | the raw response of the last fetch |
| `PSPDX.BMP` | the catalog screen, 480x272, 24-bit |
| `PSPDX2.BMP` | the screen after an install |
| `PSPDX.INSTALL` | a manifest URL here installs that app unattended, for testing |
| `PSP/PSPDX/cache/<id>.png`, `.mp4` | a picture once fetched, so it costs one handshake per install, not per run; what is here is shown even if the catalog does not link it -- the rig plants the repo's clips this way before a deploy |
| `PSP/PSPDX/font/ltn8.pgf` | never written by the client: where it looks for the system font when `flash0:` has none |
| `PSP/PSPDX/db/<id>.json` | what was installed: rev, directory, manifest URL |
