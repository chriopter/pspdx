# Updates

## The check

One request per installed package, against the manifest URL remembered at
install time. Compare integers:

| Local | Manifest | Result |
|---|---|---|
| `rev = 14` | `rev = 16` | offer the update |
| `rev = 16` | `rev = 16` | nothing to do |
| `rev = 16` | `rev = 14` | ignore — no downgrades |
| `rev = 14` | `yanked` | warn instead of offering |

Only installed packages are checked, so the cost scales with what the user has —
realistically ten to forty things, not the whole catalog.

## Why hosts matter more than requests

The TLS handshake dominates on a 222 MHz core; the manifests themselves are a few
hundred bytes each. So:

| Shape | 30 packages | rough |
|---|---|---|
| all on one host, keep-alive | 1 handshake + 30 GETs | ~5 s |
| every author on their own host | 30 handshakes | ~30 s |
| aggregator worker | 1 request | ~1 s |

Since nearly everything lives on GitHub, the manifests come from a single host
and one connection covers them all. Grouping requests by host and reusing the
connection is twenty lines of client code and the largest single win available.

Installing is different: a release download redirects from `github.com` to
`objects.githubusercontent.com`, so two more handshakes — once per install,
irrelevant next to 42 MB of payload.

These are estimates from handshake cost and typical 802.11b throughput. Measure
thirty real fetches on real hardware before optimising further.

## The optional worker

If author-hosted manifests ever spread across many hosts, an aggregator collapses
the check to one request: the client sends `{id, rev}` pairs, the worker returns
only what changed. Note that such a worker is trusted the moment it exists:
nothing is signed, so a relay that can rewrite a manifest can rewrite the hash
in it. That is the point at which signatures stop being overkill.

Batching beats ETags here. A 304 saves bytes but not the round trip, and the
round trip is the cost.

## Publishing

For the author there is no registry step at all: the release workflow rewrites
`rev`, `url`, `sha256` and `size` in the `.pspdx` and pushes. It is visible as soon as the
CDN cache expires.

Two things worth having anyway:

- a `dev` channel, so sixteen releases in one afternoon do not each become an
  update prompt for everyone
- a "check now" button, so publishing does not involve guessing whether it landed

## Applying it

Never overwrite in place. Download to `tmp/`, verify the hash, rename the old
directory aside, rename the new one in, commit the journal, then delete the old
one. Without a mirror that surviving `.old` directory is the only rollback there
is, because upstream may have deleted the previous asset.

Filter before offering: if a new revision raises `min_ram_mb` to 64 and the user
is on a PSP-1000, it is not an update, and showing it is a bug.
