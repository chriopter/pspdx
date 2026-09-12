# The app.pspdx format

An app describes itself in one file, `app.pspdx`, in the root of its own
repository. The file names the format it is in: `schema` is
`https://github.com/chriopter/pspdx/blob/master/manifest.md`, this page. An
integer would say as much to us and nothing at all to whoever finds one of
these on a Memory Stick in ten years. It points at github.com rather than the
Pages site because GitHub redirects a renamed repository and Pages does not.
A later version of the format points somewhere else.

JSON throughout, because cJSON is in the pspdev tree and a TOML parser is not:
one parser on the device rather than two.

## The file

```json
{
  "schema":   "https://github.com/chriopter/pspdx/blob/master/manifest.md",
  "id":       "io.github.chriopter.extremetuxracer",
  "name":     "Extreme Tux Racer",
  "author":   "chriopter",
  "summary":  "Downhill racing with a penguin.",
  "category": "games",
  "license":  "GPL-2.0",
  "repo":     "https://github.com/chriopter/psp-tuxracer",

  "version":  "0.24.0",
  "rev":      1789071038,
  "url":      "https://github.com/chriopter/psp-tuxracer/releases/download/v0.24.0/etr.zip",
  "sha256":   "807b7e08e3ac6e791b12025cef617a72c920c0d98fd5d38c59d70784262c5be2",
  "size":     44048464,
  "root":     "PSP/GAME/ExtremeTuxRacer/"
}
```

Two halves, and the split is the point.

**The author's half** is written once by hand: `id`, `name`, `author`,
`summary`, `category`, `license`, `repo`. Nothing automated ever changes it.
Optional beside it: `"asset": "*-psp.zip"`, a glob naming which release file
is the package when a release carries more than one.

| field | rule |
|---|---|
| `id` | `io.github.<owner>.<name>`: the owner is the repository's, so nobody can write a file that claims another account's app; the name is the author's, lower case `[a-z0-9]`, the repository's own name with the dashes dropped by default. At most 80 characters. It names a directory on the stick and a file in the cache, and it never changes. |
| `name` | what the list shows; under 40 characters, or the row clips it |
| `author` | a name, not a URL |
| `summary` | one line, at most 60 characters: a PSP screen is 480 pixels wide |
| `category` | one of `games`, `emulators`, `apps`, `plugins`, `demos` |
| `license` | an SPDX identifier; open source only |
| `repo` | the GitHub repository the file lives in |

**The release half** is written by the release action at every release and
never by hand: `version`, `rev`, `url`, `sha256`, `size`, `root`.

| field | rule |
|---|---|
| `version` | the tag without its `v`, for display and for the record on the stick |
| `rev` | the release's `published_at` as unix seconds. Integers compare; version strings do not, and there is no telling `1.10` from `1.9` without a policy nobody agreed to. **The higher `rev` wins.** |
| `url` | the package: one `.zip` on the GitHub release with an `EBOOT.PBP` anywhere inside |
| `sha256` | of the bytes at `url`, hex; the console refuses a download that does not match |
| `size` | in bytes, the same bytes; the confirm band quotes it |
| `root` | the directory inside the zip that holds the EBOOT, as it lands under `PSP/GAME/`; a moved EBOOT is a moved package |

`rev` and `size` are read as numbers, `sha256` as 64 hex digits, `url` and
`id` as strings within their limits. Anything else in the file is ignored, so
the file may grow. Nothing is ever compared but `rev`.

## What the console does with it

The console reads the file from
`https://raw.githubusercontent.com/<owner>/<repo>/HEAD/app.pspdx`, or from a
catalog that mirrors it. A line pinned to a tag reads the copy the action
attached to that release instead,
`https://github.com/<owner>/<repo>/releases/download/<tag>/app.pspdx`: the
tag was cut before the action ran, so the tree at the tag still holds the
release before. `rev` above the record on the stick is an update;
equal is current; no record is *not installed*. An install fetches `url`,
checks `sha256` and `size`, unpacks `root` under `PSP/GAME/`, and writes a
record with the `id`, the `rev`, the `version` and where the file came from.
Everything else in the file is shown, never acted on.

The file's own `id` has to begin with `io.github.<owner>.` for the owner
the list named, and a catalog entry's id has to be the one the file says, or
the console refuses it: a file that claims another account's id must not be
allowed to overwrite that account's record.

## Pictures and sound: in the EBOOT

The file carries no pictures. They are where Sony put them, inside the
`EBOOT.PBP`, and the catalog takes them out of the package it verifies:

| in the PBP | on the console |
|---|---|
| `ICON0.PNG`, 144x80 | the icon at the row, the same one the XMB shows |
| `PIC1.PNG`, 480x272 | the picture on the card |
| `ICON1.PMF`, 144x80 PSMF, a few seconds | the film on the card, played as it is |
| `SND0.AT3`, ATRAC3, a short loop | played under the card while the cursor rests on the app |

Leave one out and the console shows nothing there; it never spends a request
discovering that. `pack-pbp` takes all four beside `PARAM.SFO` and `DATA.PSP`.
[`app/tools/eboot-media/`](app/tools/eboot-media/) turns an MP4 into a
conformant `ICON1.PMF` and a WAV into `SND0.AT3`.

## The release action

A workflow of three lines in the author's repository, on `release: published`,
runs [`chriopter/pspdx/action`](action/). It picks the zip (the only one, or
the one `asset` names), downloads it, hashes it, finds the EBOOT, writes the
release half into `app.pspdx`, commits the file to the default branch and
attaches a copy to the release. The author's half is read from the file that
is already there; a repository without one gets a first draft from the
repository's own name, owner, description and licence, to be corrected by
hand.

## Lists and catalogs

Where the console finds apps is a *list*: a text file, one GitHub repository
a line.

```
# repos.txt -- one repository a line; @tag pins a release
cache https://chriopter.github.io/pspdx-catalog/catalog.json
https://github.com/chriopter/psp-tuxracer
https://github.com/chriopter/psp-rust-raytracer@v0.4.1
```

Given a list, the console fetches every repository's `app.pspdx` from
`raw.githubusercontent.com`: one host, one small file a repository. (Today
each file is its own connection and costs a handshake, about a fifth of a
second; keeping the connection open across them is the obvious next step,
and the cache makes it one request either way.)
A `cache` line names a catalog that has done that already and mirrors the
pictures: a `catalog.json` whose `apps` carry the same fields, a `release`
object with the release half, and `icon`, `screenshot`, `video`, `sound`
URLs. The console takes the cache when it answers and walks the list when it
does not. A cache proves nothing; the console checks the zip against the
`sha256` in the file either way.

Anyone can publish a list. [pspdx-catalog](https://github.com/chriopter/pspdx-catalog)
is the one the console ships with, together with the workflow that builds
its cache every hour. The console keeps its lists in `PSP/PSPDX/sources.txt`,
one URL a line, the built-in one first; more come in through the gear tab,
and so does a single repository, which is a list of one. The first list to
name an id wins.

## What was left out

**Signatures.** A sha256 in the file only proves that the bytes are the ones
the author's action hashed. The trust is in the release on the author's own
GitHub account, and in whoever wrote the list.

**Dependencies, requirements, screenshots as fields.** Homebrew has no shared
libraries to depend on, every PSP that runs a browser runs any of these, and
the pictures are in the EBOOT already.

**Version comparison.** `rev` is a number so that no policy is needed.
