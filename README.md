# PSPDX

**PSP Download Index** — the homebrew package index for the PlayStation Portable.

The client fetches [the catalog](https://chriopter.github.io/pspdx/catalog.json),
installs a package and checks for updates. It runs under PPSSPP; nobody has put
it on real hardware yet.

## What it does

| | |
|---|---|
| ![The catalog, one app listed with category and licence](docs/media/catalog.png) | **Fetch the catalog.** One request, parsed with cJSON. About 200 bytes per entry, so a thousand apps is one 200 KB fetch. |
| ![An install finishing: 639 files, 49998K, 53 seconds](docs/media/install.png) | **Install a package.** 44 MB through two redirects, sha256 verified, 639 files unpacked, then a single rename into `PSP/GAME/`. |
| ![The same app now showing an available update to 0.16.0](docs/media/update.png) | **Check for updates.** One request per installed package to the author's own manifest, comparing `rev` as a number. |

Screenshots come from the app itself, which dumps its framebuffer to the stick.
A real PSP has no screen capture, and neither does a host whose desktop is
locked.

## The two files

An app is a catalog entry here and a manifest in its author's repository.
Nothing else. [docs/manifest.md](docs/manifest.md) is the long version, and
what every `schema` field points at.

<details>
<summary><b>Catalog entry</b> — what exists. Lives here, in <code>catalog/apps/&lt;id&gt;/app.json</code>.</summary>

| Field | |
|---|---|
| `id` | reverse-DNS, stable forever, also the directory name |
| `name`, `summary` | what the client lists; summary fits in one PSP line |
| `author` | who publishes the PSP build, not the upstream project |
| `category` | `games`, `emulators`, `apps`, `plugins`, `demos` |
| `license` | SPDX id, or `proprietary` |
| `repo` | the project, and by convention where its `app.pspdx` lives |

Never a version: the entry says what exists, not what is current, which is why
a stale catalog costs nothing. An optional `icon.png` or `screenshot.png` in
the same directory is picked up by name.

**Template: [`docs/templates/app.json`](docs/templates/app.json)**

</details>

<details>
<summary><b><code>app.pspdx</code></b> — what is current. Lives in the author's repository.</summary>

| Field | |
|---|---|
| `schema` | the URL of the document describing this format |
| `id` | the same id as the catalog entry |
| `rev` | unix seconds, set by the publish step; the only field compared |
| `url` | the release archive, rooted at `PSP/` |
| `sha256`, `size` | what the client verifies, and what it warns about first |
| `requires` | `ram_mb`, so a 64 MB package is not offered to a PSP-1000 |
| `display` | shown, never acted on: `version`, `notes` |

The client looks for `app.pspdx` on the repository's default branch, so a
catalog entry names a manifest only when the file is somewhere else.

**Template: [`docs/templates/app.pspdx`](docs/templates/app.pspdx)**

</details>

## Where the keys come from

<img src="docs/media/entropy-sweep.gif" width="480" alt="A 60x28 grid of cells filling with green as the analog stick sweeps across it">

The console has no usable randomness of its own. `getentropy()` in pspsdk is
MT19937 reseeded from `time(NULL)` on every call, and the hardware generator in
the crypto chip is a kernel-only export a user-mode EBOOT cannot import at all.
So the entropy comes from a moving thumb, the same ritual PGP and TrueCrypt
performed with the mouse — the one source that is not part of the machine's own
determinism, and the one that works identically on hardware and in an emulator.

Covering a field rather than counting movements is deliberate: nervous wiggling
parks the stick against a stop where the ADC saturates and stops saying
anything, and a worn stick that drifts on its own would fill a naive counter but
not a grid. A run credits two bits per changed sample and reaches 256 bits in a
few seconds. The result is written to the Memory Stick, so the ritual happens
once. The recording above is a replayed input trace, which is why it says the
entropy in it is not real.

## Layout

| | |
|---|---|
| [`app/`](app/) | the on-device client: TLS 1.3, entropy off the analog stick, install |
| [`catalog/`](catalog/) | one directory per app, folded into the `catalog.json` the client fetches |
| [`docs/`](docs/) | the design notes, including the approaches that were dropped and why |

Updates never go through the catalog. Each package's `app.pspdx` lives in its
author's own repository and is the only thing compared, which is why a stale
catalog costs nothing. Nothing is signed, and
[open-questions.md](docs/open-questions.md) argues why that is the right amount
of trust for now.
