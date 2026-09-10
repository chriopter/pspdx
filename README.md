# PSPDX

**PSP Download Index** — the missing package manager for PSP homebrew.

Gives you a catalog (living in this repo) of sick & current Brews for the PSP including updating them from a central place!

<img src="media/catalog.png" width="480" alt="The catalog, one app listed with category and licence">

To get your open source licensed brew listed, send a PR or open an issue.

## Technical
- PSPDX polls a catalog / index of apps in this repository, each one pointing at the author's own repository and a manifest file, where the download lives.
- TLS: PSPDX supports TLS1.3, Seedgeneration (bc. missing PRNG) on start.
- Fast connection to all vendor repos by reusing initial TLS handshake with github

## The two files

An app is a catalog entry here and a manifest in its author's repository.
[manifest.md](manifest.md) is the long version.

<details>
<summary><b>Catalog entry</b> — what exists. Here, in <code>catalog/apps/&lt;id&gt;/app.json</code>.</summary>

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

Never a version, which is why a stale catalog costs nothing. An `icon.png` or
`screenshot.png` in the same directory is picked up by name.

Copy: [`app.json.template`](app.json.template)

</details>

<details>
<summary><b><code>app.pspdx</code></b> — what is current. In the author's repository.</summary>

```json
{
  "schema": "https://github.com/chriopter/pspdx/blob/master/manifest.md",
  "id": "io.github.chriopter.extremetuxracer",
  "rev": 1789034382,
  "url": "https://github.com/chriopter/psp-tuxracer/releases/download/v0.16.0/extremetuxracer-psp.zip",
  "sha256": "2cd0a663535b613a2449fcd68c111b9f4301465aefdda4c6c3fce33e5cd8d231",
  "size": 44048460,
  "requires": { "ram_mb": 64 },
  "display": {
    "version": "0.16.0",
    "notes": "First release listed in PSPDX."
  }
}
```

`rev` is unix seconds and the only field compared. Found at `app.pspdx` on the
repository's default branch, so a catalog entry names a manifest only when the
file is elsewhere.

Copy: [`app.pspdx.template`](app.pspdx.template)

</details>
