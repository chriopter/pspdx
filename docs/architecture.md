# Architecture

Two layers that answer two different questions.

| Layer | Question | Lives | May be stale |
|---|---|---|---|
| Package manifest | "is there a newer version of what I have?" | in the author's own repo, as `<name>.pspdx` | no — it is the truth |
| Catalog | "what else exists?" | in the registry repo, as `catalog.json` | yes, a day is fine |

Splitting them this way is what makes the thing simple. Updates never travel
through the registry, so the registry is allowed to be slow, hand-curated and
occasionally wrong without anyone noticing.

## Package manifest — decentralised

Each project ships a `.pspdx` file in its own repository. The author's release
workflow rewrites the version line; that is the entire publishing process. On
install, the client remembers the manifest URL and asks that URL, and only that
URL, about later versions.

This is the Sparkle appcast model, and the same shape Chrome extensions use with
`update_url`. Trust becomes per-package instead of central: nobody can push an
update for someone else's software, and there is no registry to compromise.
What backs it today is TLS and the author's control of their own repository,
not a signature — see [open-questions.md](open-questions.md).

Downside: it can only update what is already installed. On its own it is an
updater, not a package manager.

## Catalog — central, optional

The catalog exists for discovery, curation and compatibility data. It is a
generated `catalog.json` committed to a git repository and fetched directly from
there:

```
https://cdn.jsdelivr.net/gh/<owner>/<repo>@master/catalog.json
```

No build, no deploy, no CDN of your own. Merge a pull request and it is live once
the cache expires. `raw.githubusercontent.com` serves the same file but is not a
CDN under GitHub's terms, so jsDelivr is the polite version of the same trick.

Because the catalog is not on the update path, it does not need shards, a Merkle
tree, or an incremental format. An earlier draft had all three; the per-package
manifest made them unnecessary.

## Multiple registries

The client should accept any catalog URL, each with its own key, exactly as
Universal-Updater does with `.unistore` files on the 3DS and Homebrew does with
taps. Then this project is the default, not the authority — and anyone who
disagrees with a curation decision can run their own instead of arguing.

## What was dropped, and why

- **Signed head plus Merkle-rooted shards.** Built to make a huge catalog
  cheap to poll incrementally. Once updates moved to the per-package manifest,
  the catalog stopped being polled often enough to justify it.
- **`repository_dispatch` into the registry on every release, to make a push
  visible within two minutes.** A whole pipeline to cache a fact that GitHub
  already serves. The author's own manifest is visible immediately and needs
  none of it.
- **Mirroring every artifact on merge.** See [open-questions.md](open-questions.md);
  the GPL source obligation makes this much more expensive than it looks.
