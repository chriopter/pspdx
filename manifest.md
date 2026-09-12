# Lists, caches, and the EBOOT

An app is a GitHub repository with a release, and the release carries one
zip with an `EBOOT.PBP` in it. That is all an author does, and it is what
authors were doing already. Nothing is written for PSPDX: not a file, not a
workflow, not a line of CI. Everything the console shows is derived from
what is there.

## Where every field comes from

| field | source |
|---|---|
| `id` | the repository URL: `io.github.<owner>.<repo>`, lower case, `[a-z0-9]` only in the last part, dashes and dots dropped. It names a directory on the stick and a file in the cache, and it never changes. |
| `name` | `TITLE` in the `PARAM.SFO` inside the EBOOT |
| `author` | the repository's owner |
| `summary` | the repository's description, cut to 60 characters: a PSP screen is 480 pixels wide |
| `license` | what GitHub reports for the repository (an SPDX id); empty when it reports none |
| `category` | the list: the word after the URL |
| `version` | the release tag without its `v` |
| `rev` | the release's `published_at` as unix seconds. Integers compare; version strings do not. **The higher `rev` wins.** |
| `url`, `size` | the zip on the release: the only `.zip`, or the one an `asset=` glob on the list names |
| `sha256` | computed by whoever downloads the whole zip: the cache. The console checks it when a cache gave it, and the size otherwise. |
| `root` | the directory of the shallowest `EBOOT.PBP` in the zip, as it lands under `PSP/GAME/` |
| icon, picture, film, sound | `ICON0.PNG` (144x80), `PIC1.PNG` (480x272), `ICON1.PMF` (144x80 PSMF), `SND0.AT3` (ATRAC3) inside the EBOOT, where Sony put them |

Nothing is ever compared but `rev`.

## The list

A list is a text file, one GitHub repository a line, with its category:

```
# repos.txt: one repository a line, then its category
cache https://chriopter.github.io/pspdx-catalog/catalog.json
https://github.com/chriopter/psp-tuxracer games
https://github.com/chriopter/psp-cathedral demos
https://github.com/chriopter/psp-rust-raytracer@v0.4.2 demos
https://github.com/someone/psp-thing apps name="The Thing" license=MIT
```

`category` is one of `games`, `emulators`, `apps`, `plugins`, `demos`.
`@tag` pins a release. After the category, `key=value` words override what
would be derived: `name`, `author`, `summary`, `license`, `asset` (a glob
naming the zip when a release carries more than one); a value with spaces
is quoted. Overrides are the curator's, for the title that is an
abbreviation in the SFO or the licence GitHub cannot see. Anyone can publish
a list; a file on any web server will do. Comments start with `#`.

A `cache` line names a catalog that has done the deriving already.

## The cache

A workflow at the list's repository, hourly and on request, walks the list:
asks GitHub for each repository and its latest release, downloads the zip,
hashes it, finds the EBOOT, reads the title out of the SFO and the pictures
and the sound out of the PBP, and writes one `catalog.json` with the
pictures beside it under names that carry their bytes
(`icons/<id>-<sha8>.png`). It is stateless: nothing is committed, the next
run derives it all again.

The bar for the cache is the bar Sony set for a PBP: a title, `ICON0.PNG`
and `PIC1.PNG`. A release that clears it is listed; one that does not is
reported with the reason and left out, and the curator can tell the author.
`ICON1.PMF` and `SND0.AT3` are welcome and not required.
[`app/tools/eboot-media/`](app/tools/eboot-media/) makes those two from an
MP4 and a WAV, for authors who want the card to move and sound.

`catalog.json` is `{"schema", "generated", "apps": [...]}`; each app carries
`id`, `name`, `author`, `summary`, `category`, `license`, `repo`,
`release` (`rev`, `url`, `sha256`, `size`, `version`) and `icon`,
`screenshot`, `video`, `sound` where the EBOOT had them. A cache may leave
any of the pictures out; the console fills what it can from elsewhere.

## The console

`PSP/PSPDX/sources.txt` holds the lists, one URL a line, the built-in one
first; a repository URL is a list of one, and that is how a single app in no
catalog gets in, typed as `owner/repo` on the firmware's keyboard. For every
list: the cache is taken when it answers; every repository the cache did not
cover, and every repository when there is no cache, is asked at the origin.
The first list to name an id wins.

**At the origin** the console asks GitHub's API for the repository and its
release: `api.github.com/repos/<owner>/<repo>` and `.../releases/latest`
(or `.../releases/tags/<tag>`), two small JSON files a repository, over
TLS. That gives name (the repository's, until the SFO's is on the stick),
author, summary, licence, version, rev, the zip and its size. No hash: the
console checks the size of what it downloads and nothing else, and the zip
comes from the author's own account over TLS, which is the trust there is.
No pictures before the install. Sixty such calls an hour are allowed to one
address without a token; a cache spends none.

**On the stick** an installed app carries its own EBOOT, and the console
reads `ICON0`, `PIC1`, `ICON1` and `SND0` straight out of it, so what is
installed is pictured whether or not any cache ever was.

An install fetches the zip, checks the size (and the hash when it has one),
unpacks the EBOOT's directory under `PSP/GAME/`, and writes
`PSP/PSPDX/db/<id>.json`: `id`, `rev`, `dir`, `version`, `repo`. An update
is a larger `rev` for the same id from any source.

## What was left out

**A file in the author's repository.** It was here twice, as a manifest and
as `app.pspdx`, and both asked the author to maintain something for us.
Everything in it turned out to be derivable, and what is derivable goes
wrong when people have to keep it. If an author ever wants to say something
GitHub and the SFO cannot, a line on the list can say it for them; a file
of their own could be added later as an override without breaking anything.

**Signatures.** The trust is in the release on the author's own account and
in whoever wrote the list.

**Version comparison.** `rev` is a number so that no policy is needed.
