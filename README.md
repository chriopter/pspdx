# PSPDX

**PSP Download Index** — the homebrew package index for the PlayStation Portable.

The client fetches [the catalog](https://chriopter.github.io/pspdx/catalog.json),
installs a package and checks for updates. It runs under PPSSPP; nobody has put
it on real hardware yet.

That URL is compiled into the EBOOT; everything behind it is static, so nothing
has to keep running. A PSP pays per handshake, not per byte -- the whole index
is one request.

## What it does

<img src="docs/media/catalog.png" width="480" alt="The catalog, one app listed with category and licence">

One request fetches the catalog. Installing verifies the sha256, unpacks, and
commits with a single rename into `PSP/GAME/`. Each installed package is
checked against its author's manifest, comparing `rev`.

The app dumps its own framebuffer to the stick; a PSP has no screen capture.

## The two files

An app is a catalog entry here and a manifest in its author's repository.
[docs/manifest.md](docs/manifest.md) is the long version.

<details>
<summary><b>Catalog entry</b> — what exists. Here, in <code>catalog/apps/&lt;id&gt;/app.json</code>.</summary>

| Field | |
|---|---|
| `id` | reverse-DNS, stable forever, also the directory name |
| `name`, `summary` | what the client lists; summary fits one PSP line |
| `author` | who publishes the PSP build, not the upstream project |
| `category` | `games`, `emulators`, `apps`, `plugins`, `demos` |
| `license` | SPDX id, or `proprietary` |
| `repo` | the project, and by convention where its `app.pspdx` lives |

Never a version, which is why a stale catalog costs nothing. An `icon.png` or
`screenshot.png` in the same directory is picked up by name.

Template: [`docs/templates/app.json`](docs/templates/app.json)

</details>

<details>
<summary><b><code>app.pspdx</code></b> — what is current. In the author's repository.</summary>

| Field | |
|---|---|
| `schema` | the URL of the document describing this format |
| `id` | the same id as the catalog entry |
| `rev` | unix seconds, set by the publish step; the only field compared |
| `url` | the release archive, rooted at `PSP/` |
| `sha256`, `size` | what the client verifies, and what it warns about first |
| `requires` | `ram_mb`, so a 64 MB package is not offered to a PSP-1000 |
| `display` | shown, never acted on: `version`, `notes` |

Found at `app.pspdx` on the repository's default branch, so a catalog entry
names a manifest only when the file is elsewhere.

Template: [`docs/templates/app.pspdx`](docs/templates/app.pspdx)

</details>

## Where the keys come from

<img src="docs/media/entropy-sweep.gif" width="480" alt="A 60x28 grid of cells filling with green as the analog stick sweeps across it">

The console has no usable randomness of its own: `getentropy()` in pspsdk is
MT19937 reseeded from `time(NULL)`, and the hardware generator is a kernel-only
export. So the entropy comes from a moving thumb, the one source that is not
part of the machine's own determinism. Covering a field rather than counting
movements defeats both a stick parked against a stop and one that drifts on its
own. 256 bits takes a few seconds and is saved, so the ritual happens once. The
recording above is a replayed trace, so its entropy is not real.

## Layout

| | |
|---|---|
| [`app/`](app/) | the on-device client: TLS 1.3, entropy off the analog stick, install |
| [`catalog/`](catalog/) | one directory per app, folded into the `catalog.json` the client fetches |
| [`docs/`](docs/) | the design notes, including the approaches that were dropped and why |

Nothing is signed; [open-questions.md](docs/open-questions.md) argues why that
is the right amount of trust for now.
