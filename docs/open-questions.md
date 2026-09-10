# Open questions

## Signing — decided against, for now

Nothing is signed. TLS 1.3 to GitHub carries the trust, and that is the whole
arrangement:

| | who is trusted | what it takes to lie |
|---|---|---|
| catalog | us, over HTTPS | our repository, or a certificate for our host |
| manifest | the author, over HTTPS | the author's repository, or one for theirs |
| artifact | the manifest's `sha256` | see above — the hash is pinned by the manifest |

The hash is what actually protects the 42 MB, and it costs nothing. Signatures
would protect against a compromised transport, and there is no transport left
to compromise while everything comes from one origin over TLS.

The reason to stop here rather than build it anyway: a signature is only worth
its complexity once the key can be verified independently of the thing it
signs. Today the client would have to learn the author's key from the catalog,
which it fetches over the same TLS from the same GitHub that already serves the
manifest. That is a signature that proves nothing the transport did not already
prove, plus a key-rotation problem nobody has.

Two things would change the answer. Mirroring, because a mirror that can rewrite
bytes is exactly the untrusted middle a signature is for. And a second person
with write access to the catalog, because then "trust us" stops being one
person's word.

Anti-rollback does not need signatures and is already in: the client stores the
`rev` it installed and refuses anything lower. It must not rely on the clock —
the PSP's RTC is user-settable and resets when the battery dies, so expiry
checks are worthless.

## Mirroring, and what GPL does to it

Without a mirror the index costs nothing to run, and link rot eventually eats it.
With a mirror it survives, and the bill is larger than it first appears:

| | index only | with mirror |
|---|---|---|
| storage per TuxRacer revision | 0 | ~280 MB |
| pinned hash | observed, can change under you | permanent |
| dead upstream | package is gone | unaffected |
| **GPL source obligation** | **not yours** | **yours, 240 MB per revision** |

That last row is the one that changes the calculation. Distributing a GPL binary
means distributing the source with it; linking to it does not. One active project
can therefore cost more storage than a hundred frozen homebrews from 2009.

A middle path: mirror on death rather than on merge. Keep only health data at
first, and fetch a copy — from the Wayback Machine, or from a user — at the point
where an artifact actually disappears. Storage then grows with decay rather than
with the size of the catalog.

Note also that GitHub release assets are already a free blob store with a 2 GB
per-file limit, so mirroring never requires renting anything.

## Permission to mirror

Two tiers, and an open-source licence settles most of it without asking anyone:

| Licence | Tier |
|---|---|
| MIT, GPL, zlib, … | mirror — the licence already grants it |
| "freeware", nothing stated | index only, upgraded on request |
| author objects | index only, permanently |

The tiers need different hash policies. A mirrored artifact can be pinned
forever. An upstream one cannot, because re-uploading "v1.0 FIXED" over the same
URL is common in this scene — so record the last observed hash and warn on
change rather than refusing to install.

## Curation

Not hosting bytes decentralises storage, not authority. Whoever decides what gets
listed, whether a kernel plugin bricks a console, and when a revision is pulled
is still a single point — and that work is the part that makes an index worth
more than a list of links.

Supporting multiple catalog URLs is the honest answer: this project becomes the
default rather than the authority, and disagreement can be forked instead of
argued.

The other line that has to hold from day one: the scene mixes homebrew with ISOs
and BIOS images. Licence and source as mandatory fields, and a `user_data` prompt
for anything that cannot be shipped — a Quake `pak0.pak`, ScummVM game data —
keep this an index rather than a warez search engine. It is also what keeps
mirrors willing to host it.

## One repository or two

Decided: one. The catalog lives in `catalog/` inside this repository.

The split only starts paying for itself once a bot has write access to the
catalog and the client embeds a root key, because those two things must not sit
in the same place. Neither exists yet, and nothing has shipped, so the move costs
nothing today.

It will not stay free. Once a client is released the catalog URL is baked into
it, and relocating the catalog afterwards points every installed copy at a dead
URL. Either split before the first release, or publish the catalog through a
jsDelivr URL, which can be redirected to another repository without the client
noticing.
