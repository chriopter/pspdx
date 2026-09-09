# Manifest sketch

Two files, deliberately different in who owns them.

## `<name>.pspdx` — in the author's repository

The authoritative statement of "what is current". The author owns it, the
release workflow rewrites the `[current]` block, nothing else touches it.

```toml
id      = "de.chriopter.extremetuxracer"
name    = "Extreme Tux Racer"
license = "GPL-2.0"
key     = "ed25519:MCowBQYDK2VwAyEA..."   # pinned by the client on first install

[requires]
min_ram_mb = 32
devices    = ["phat", "slim", "brite", "go"]
firmware   = ">=6.60"

[current]
rev     = 16                       # monotonic, the only thing compared
version = "0.15.0"                 # cosmetic
url     = "https://github.com/chriopter/psp-tuxracer/releases/download/v0.15.0/extremetuxracer-psp.zip"
sha256  = "..."                    # already published as SHA256SUMS
size    = 44040192
source  = ".../sources.tar.gz"     # GPL: not optional
notes   = "..."                    # release body, trimmed
channel = "stable"                 # "dev" keeps rapid iteration out of the way
```

`rev` is a plain integer and the only field used to decide whether something is
newer. Homebrew version strings are chaos — `r12`, `v0.9b`, `final2`, bare dates
— and no ordering can be derived from them. Keeping `rev` separate also means no
version parser has to run on the console, and a downgrade cannot be expressed.

## Catalog entry — in the registry

The registry stores what rarely changes plus a pointer, and never a version:

```json
{
  "id": "de.chriopter.extremetuxracer",
  "name": "Extreme Tux Racer",
  "summary": "Downhill racing with a penguin.",
  "category": "games",
  "license": "GPL-2.0",
  "manifest": "https://cdn.jsdelivr.net/gh/chriopter/psp-tuxracer@master/extremetuxracer.pspdx",
  "size_hint": 44040192,
  "last_seen_ok": "2026-09-09T10:44:47Z"
}
```

Keeping the version out of the catalog is the point: the catalog cannot go stale
in a way that matters, because it never claimed to know the current version.

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
