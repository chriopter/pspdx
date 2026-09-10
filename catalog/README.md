# Catalog

One file per app in `apps/`, named after the app id. `build.py` folds them
into the `catalog.json` the client fetches; the Pages workflow runs it on every
push.

The catalog answers "what exists". It never carries a version: what is current
lives in the author's `.pspdx` manifest, which is why a stale catalog is
harmless.

| Field | |
|---|---|
| `id` | reverse-DNS, stable forever, also the file name |
| `name`, `summary` | what the client lists; summary fits in one PSP line |
| `author` | who publishes the PSP build, not the upstream project |
| `category` | `games`, `emulators`, `apps`, `plugins`, `demos` |
| `license` | SPDX id, or `proprietary` -- mandatory, see docs/open-questions.md |
| `homepage` | where a human goes |
| `manifest` | where the client goes for updates |
