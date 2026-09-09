# Open questions

## Signing

If the catalog is fetched straight from a git repository there is no build step,
and therefore nothing signs it. Three options, roughly in order of effort:

1. trust HTTPS and GitHub — entirely defensible to start with
2. a bot commits a `catalog.json.sig` alongside, which is the smallest possible
   remnant of a pipeline
3. the full offline-root arrangement, only worth it once someone other than the
   author can write to the registry

Package manifests are a separate matter and should be signed by their author from
the beginning, with the key pinned by the client at install time. That is what
makes per-package trust work without any central authority.

Anti-rollback should not rely on the clock. The PSP's RTC is user-settable and
resets when the battery dies, so expiry checks are worthless. Storing the highest
`rev` ever seen and refusing anything lower achieves the same thing without
needing to know the date.

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

Currently split into `pspdx` and `pspdx-registry`. The split matters once a bot
has write access to the catalog and the client embeds a root key, because those
two things must not live in the same place. Before that it is overhead.

The decision has a deadline, though: once a client ships, the catalog URL is
baked into it. Moving the catalog afterwards points every installed copy at a
dead URL. Deciding before the first release costs nothing; after it costs a
migration.
