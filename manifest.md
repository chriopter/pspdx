# The catalog format

An entry names the format it is in: `schema` is
`https://github.com/chriopter/pspdx/blob/master/manifest.md`, this file. An
integer said as much to us and nothing at all to whoever finds one of these on
a Memory Stick in ten years. It points at github.com rather than the Pages
site because GitHub redirects a renamed repository and Pages does not. A later
version of the format points somewhere else.

JSON throughout, because cJSON is in the pspdev tree and a TOML parser is not —
one parser on the device rather than two.

## An entry

One directory per app in [pspdx-catalog](https://github.com/chriopter/pspdx-catalog),
named after the id. Two files, and the split is the point.

`app.json` — a person's. Nothing automated ever writes it.

```json
{
  "id": "io.github.chriopter.extremetuxracer",
  "name": "Extreme Tux Racer",
  "author": "chriopter",
  "summary": "Downhill racing with a penguin.",
  "category": "games",
  "license": "GPL-2.0",
  "repo": "https://github.com/chriopter/psp-tuxracer"
}
```

`latest.json` — the scanner's, rewritten whole, never edited by hand.

```json
{
  "rev": 1789046256,
  "seen": 1789071038,
  "version": "0.24.0",
  "url": "https://github.com/…/extremetuxracer-psp.zip",
  "sha256": "9f2c1e4b8a7d…",
  "size": 44048460,
  "root": "PSP/GAME/ExtremeTuxRacer/"
}
```

| Field | |
|---|---|
| `id` | reverse-DNS, stable forever, also the directory name |
| `name`, `summary` | what the client lists; summary fits one PSP line |
| `author` | who publishes the PSP build, not the upstream project |
| `category` | `games`, `emulators`, `apps`, `plugins`, `demos` |
| `license` | SPDX id, or `proprietary` |
| `repo` | the project |
| `asset` | optional: a glob, when a release attaches more than one file |
| `scan` | optional: `false` pins the entry where it is |
| `rev`, `version`, `url`, `sha256`, `size` | the release, and what the console needs |
| `seen`, `root` | how the scanner recognises the next release; never served |

The id comes from a domain the author controls, reversed. Without one, GitHub
supplies it: `io.github.<user>.<app>`, from `github.io`, the same rule Flathub
uses. Stable forever means exactly that — it is the directory on the Memory
Stick and the key in the install journal, so changing it orphans installs.

A bot commits into `latest.json` every hour and people edit `app.json` by pull
request. Keeping them in one file meant merge conflicts between the two, and a
diff nobody could skim; the workflows stage `apps/*/latest.json` and nothing
else, so the separation is enforced rather than promised.

## Why `rev` is a number, and why there are two of them

Unix seconds, and the only field compared. Homebrew version strings are chaos — `r12`, `v0.9b`, `final2`,
`1.0 FIXED` — and no ordering can be derived from them. Keeping `rev` separate
also means no version parser runs on the console, and a downgrade cannot be
expressed. `version` sits beside it to be printed and is never compared.

`rev` is the revision of the bytes and `seen` is the release the scanner last
looked at. They differ whenever an author re-tags the same build: `seen` moves,
`rev` does not, and nobody is offered an update that would download what they
already have. `seen` also takes the newest of the release's own timestamp and
its assets', because deleting an asset and uploading a replacement under the
same tag leaves `published_at` untouched — and used to leave the catalog
serving a checksum for bytes that were gone.

`sha256` is what actually protects the payload, and `size` is mandatory rather
than a convenience — 42 MB over 802.11b is minutes, and that belongs in front
of the download rather than behind it.

## Assets

Convention, not fields: drop `icon.png` (the 144x80 `ICON0.PNG` out of the
EBOOT), `screenshot.png` (480x272) or `video.mp4` into the app's directory and
the generated entry gains an `icon`, `screenshot` or `video` path. Leave one
out and the entry has none, so the console never spends a request discovering
that there is nothing there. All three are fetched per app and only for the app
on screen — a list of fifty stays one request.

Ten seconds of video is about 750 KB. The PSP decodes H.264 baseline in
hardware on the Media Engine, which is what UMD Video shipped, so nothing else
will play:

```sh
ffmpeg -i recording.avi -ss 3 -t 10 \
  -vf "scale=480:272:flags=lanczos,fps=30" \
  -c:v libx264 -profile:v baseline -level 3.0 -pix_fmt yuv420p \
  -b:v 600k -maxrate 800k -bufsize 1200k \
  -movflags +faststart -an video.mp4
```

Baseline rules out B-frames, `yuv420p` is the only chroma the decoder takes,
and 480x272 at 30 fps is the panel. Audio is dropped: the catalog is browsed
with the speaker off as often as not. Recordings come from PPSSPP with
`DumpFrames = True`, which captures what the GE renders — an app that writes
its framebuffer directly records as noise and needs a screenshot instead.

## `app.pspdx` — optional, in the author's repository

An author who would rather not wait for the next scan can keep the same release
fields in a file of their own, at `app.pspdx` on the default branch:

```json
{
  "schema": "https://github.com/chriopter/pspdx/blob/master/manifest.md",
  "id":     "io.github.chriopter.extremetuxracer",
  "rev":    1789034687,
  "url":    "https://github.com/…/extremetuxracer-psp.zip",
  "sha256": "9f2c1e4b8a7d…",
  "size":   44048460,
  "requires": { "ram_mb": 64 },
  "display": { "version": "0.21.0", "notes": "Fixes the crash on large courses." }
}
```

This is not wired up: the scanner does not look for the file and the entry
gains no `manifest` field, so nothing on the console asks for one. The shape is
here because the console still reads a `manifest` field if an entry carries
one, and because an author who publishes releases often is the case that would
justify building it.

**The higher `rev` wins.** A stale manifest is therefore harmless, and they do
go stale: `psp-tuxracer` shipped one saying 0.16.0 while its releases were at
0.21.0. Taking part helps, not taking part costs nothing, taking part badly
costs nothing either.

Everything under `display` is shown and never acted on: never parsed, never
compared, never part of a decision. That turns a convention into a checkable
boundary, and makes it the safe place for later additions like an author or an
icon.

## Scanning

Nobody types a sha256. `scan.py` asks GitHub for the newest release,
downloads the archive, hashes the bytes it actually received, and looks inside
to see where the EBOOT sits.

```sh
python3 scan.py https://github.com/user/repo   # add it, or refresh it
python3 scan.py --all                          # refresh everything
python3 scan.py --all --check                  # say what would change
```

It runs hourly in Actions, one small API call per app. Conditional requests
were tried and dropped: the release JSON carries each asset's `download_count`,
so its ETag changes whenever anybody downloads anything and the 304 almost
never arrives. `"scan": false` pins an entry where it is.

Assuming a fixed archive layout does not survive contact with PSP homebrew: of
sixteen surveyed release archives that contain an EBOOT, ten put it one
directory down, three at the root, and two under `PSP/GAME/`. So the shallowest
`EBOOT.PBP` wins and its directory is the package. If a new release moves that
directory the entry is **not** updated — it keeps the release known to install,
and the scan opens an issue.

An author with no access can ask for a rescan by opening an issue titled
`rescan: <repo url>`, which is acted on only for repositories already listed.
From a release workflow:

```yaml
- run: gh issue create -R chriopter/pspdx-catalog
       -t "rescan: ${{ github.repository }}" -b ""
  env:
    GH_TOKEN: ${{ secrets.PSPDX_PING }}
```

The token is the author's own, in the author's own repository. Nothing of ours
is shared, and it grants no access to the index.

## What was left out

`channel`, because there is one channel. `source`, because the GPL source only
matters if we mirror, which we do not, and the author's release page carries it
anyway. `root` as something an author fills in, replaced by a rule the scanner
applies. And no `key`: nothing is signed.

`PARAM.SFO` inside the EBOOT already carries the title, category and required
system version, and `ICON0.PNG` is the icon. Deriving them would mean existing
homebrew could be indexed without anyone writing a description at all, which
matters more for adoption than any format decision.
