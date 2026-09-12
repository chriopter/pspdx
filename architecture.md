# Architecture

PSPDX is an index, not a host. The truth about an app is its repository and
its release; the index is a cache of what they say, derived, never
declared. An author writes nothing for PSPDX.

The principle everything follows from: **poll where it is free, ask once where
it is expensive.** A 333 MHz handheld on 802.11b pays over a second for a TLS
handshake. GitHub Actions pays nothing for the same question. So the hundred
questions are asked in CI, and the console asks one, and can still ask the
hundred itself over a single connection when no cache answers.

```mermaid
flowchart TD
    A["Author cuts a release<br/>a zip with an EBOOT in it"] --> L[("repos.txt<br/>a list: one repository a line, and its category")]
    L --> C["The list's workflow, hourly<br/>asks GitHub, downloads and hashes every zip<br/>title out of the SFO, pictures out of the PBP"]
    C --> D[("catalog.json + pictures<br/>the cache, on Pages")]
    D --> E["PSP asks once, at startup"]
    L -. "no cache, or a list of one" .-> F["PSP asks GitHub's API itself<br/>two small requests a repository"]
    E --> G["Install, straight from the author's release<br/>sha256 verified"]
    F --> G
    linkStyle 4 stroke-dasharray:5
```

## Who owns what

| | |
|---|---|
| **The author** | The repository and its release: a zip with an `EBOOT.PBP`, and in the PBP the title, ICON0, PIC1, and if they like ICON1 and SND0. What they were doing anyway. |
| **A list** | Which repositories, with a category each, and the odd override. Anyone's text file. The one the console ships with is `repos.txt` in pspdx-catalog. |
| **A cache** | A workflow that reads the list, asks GitHub, verifies every package once, reads the SFO and the PBP, and publishes one `catalog.json` with the pictures. Stateless: nothing is committed, the next run derives it again. |
| **The console** | Takes the cache when it answers, asks GitHub at the origin for what it did not cover, and pictures what is installed from the EBOOT on the stick. |

## What the console does

One fetch of the cache's `catalog.json` at startup. That is the whole list,
every version, every hash, every picture's URL; the update check costs no
further request.

When there is no cache, or the source is a single repository someone typed
in, the console asks GitHub's API for the repository and its latest release:
two small requests a repository, each its own connection today. It gets no
hash that way and checks the size instead, and no pictures until the app is
on the stick, where its own EBOOT has them.

**The higher `rev` wins.** A cache that has fallen behind the author's file
is harmless: the record on the stick says what is installed, the file says
what is published, and the larger number is the update.

## Trust

The cache proves nothing and is not asked to. The zip comes from the
author's own release under the author's own account, over TLS; the cache's
`sha256` guards the transport, and without a cache the size does. A list is
trusted the way a package source is: whoever wrote it chose what is on it.
