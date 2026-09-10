# PSPDX

**PSP Download Index** — the missing package manager for PSP homebrew.

Gives you a [catalog](https://github.com/chriopter/pspdx-catalog) of sick & current Brews for the PSP including updating them from a central place!

<img src="media/catalog.png" width="480" alt="The catalog, one app listed with category and licence">

Runs under PPSSPP; nobody has put it on real hardware yet.

To get your open source licensed brew listed, send a PR to
[pspdx-catalog](https://github.com/chriopter/pspdx-catalog) or open an issue.

## Technical
- PSPDX polls a catalog / index of apps in this repository, each one pointing at the author's own repository and a manifest file, where the download lives.
- TLS: PSPDX supports TLS1.3, Seedgeneration (bc. missing PRNG) on start.
- Fast connection to all vendor repos by reusing initial TLS handshake with github

## The two files

An app is a catalog entry here and a manifest in its author's repository.
[manifest.md](manifest.md) is the long version.

<details>
<summary><b>Catalog entry</b> — what exists. In <a href="https://github.com/chriopter/pspdx-catalog">pspdx-catalog</a>, as <code>apps/&lt;id&gt;/app.json</code>.</summary>

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

Never a version, which is why a stale catalog costs nothing. An `icon.png`,
`screenshot.png` or `video.mp4` in the same directory is picked up by name --
[the catalog's README](https://github.com/chriopter/pspdx-catalog#encoding-a-video)
has the encoding the PSP can decode.

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

---

<sup>The update path is exercised against
[psp-dx-testapp](https://github.com/chriopter/psp-dx-testapp), a hello world
whose only job is to get a new version now and then. Bump its `VERSION`, cut a
release, raise `rev` in its `app.pspdx`, and the client has an update to
find -- 69 KB instead of the 44 MB of a real package.</sup>
