# The two formats

Both files name the format they are in: `schema` is `https://github.com/chriopter/pspdx/blob/master/manifest.md`,
this file. An integer said as much to us and nothing at all to whoever finds
one of these on a Memory Stick in ten years. It points at github.com rather
than the Pages site because GitHub redirects a renamed repository and Pages
does not. A later version of the format points somewhere else.

Two files, deliberately different in who owns them. Both are JSON, because
cJSON is in the pspdev tree and a TOML parser is not — one parser on the
device rather than two.

## `app.pspdx` — in the author's repository

The authoritative statement of "what is current". The author owns it, the
release workflow rewrites it, nothing else touches it.

```json
{
  "schema": "https://github.com/chriopter/pspdx/blob/master/manifest.md",
  "id":     "io.github.chriopter.extremetuxracer",
  "rev":    1789034687,
  "url":    "https://github.com/…/extremetuxracer-psp.zip",
  "sha256": "9f2c1e4b8a7d…",
  "size":   44048460,
  "requires": { "ram_mb": 64 },
  "display": {
    "version":  "0.16.0",
    "notes":    "Fixes the crash when loading large courses."
  }
}
```

`rev` is unix seconds, set by the publish step and never by hand. It is the
only field compared. Homebrew version strings are chaos — `r12`, `v0.9b`,
`final2`, `1.0 FIXED` — and no ordering can be derived from them; keeping `rev`
separate also means no version parser runs on the console, and a downgrade
cannot be expressed. A readable `20260909104447` would be prettier but does not
fit in a 32-bit integer.

Everything under `display` is shown and never acted on: never parsed, never
compared, never part of a decision. That turns a convention into a checkable
boundary, and makes it the safe place for later additions like an author or an
icon.

`sha256` is what actually protects the payload, and `size` is mandatory rather
than a convenience — 42 MB over 802.11b is minutes, and that belongs in front of
the download rather than behind it.

Three fields were considered and left out. `channel`, because there is one
channel. `source`, because the GPL source only matters if we mirror, which we do
not, and the author's release page carries it anyway. `root`, replaced by a
convention: what gets installed is the `PSP/` subtree, and a rule beats a field
every author would have to fill in correctly. And no `key`: nothing is signed
— see [open-questions.md](open-questions.md).

## Catalog entry — in the registry

The registry stores what rarely changes plus a pointer, and never a version.
The pointer is the repository, not the file: `app.pspdx` on its default branch
is the convention, so an entry names a manifest only when there is a reason to.

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

Keeping the version out of the catalog is the point: the catalog cannot go stale
in a way that matters, because it never claimed to know the current version.

One directory per app is what gets edited, under `catalog/apps/`; one generated file
is what gets served. About 200 bytes per entry, so a thousand apps is 200 KB and
one fetch.

## Metadata that need not be written twice

`PARAM.SFO` inside the EBOOT already carries the title, category and required
system version, and `ICON0.PNG` is the icon. Deriving them means existing
homebrew can be indexed without being repackaged, which matters far more for
adoption than any format decision. psp-tuxracer also ships a `build.json`, which
could supply version, commit and toolchain directly.

## Health, not just facts

The field the established registries do not have and this one needs:
`last_seen_ok`, written by a nightly HEAD request against each artifact URL, plus
the observed hash and when it last changed. Their upstreams are alive. Half of
this one's are not, so a dead link should become visible data rather than a 404
at install time.
