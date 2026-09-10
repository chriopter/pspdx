# PSPDX

**PSP Download Index** — the missing package manager for PSP homebrew.

Gives you a catalog (living in this repo) of sick & current Brews for the PSP including updating them from a central place!

<img src="docs/media/catalog.png" width="480" alt="The catalog, one app listed with category and licence">

## Technical
- PSPDX polls a catalog / index of apps in this repository, each one pointing at the author's own repository and a manifest file, where the download lives.
- TLS: PSPDX supports TLS1.3, Seedgeneration (bc. missing PRNG) on start.
- Fast connection to all vendor repos by reusing initial TLS handshake with github

## The two files

An app is a catalog entry here and a manifest in its author's repository.
[manifest.md](manifest.md) is the long version.

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

Template: [`app.json.template`](app.json.template)

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

Template: [`app.pspdx.template`](app.pspdx.template)

</details>
