# Catalog

One directory per app in `apps/`, named after the app id, holding an `app.json`
and optionally an `icon.png`. `build.py` folds them into the `catalog.json` the
client fetches and copies the icons beside it; the Pages workflow runs it on
every push.

The catalog answers "what exists". It never carries a version: what is current
lives in the author's `.pspdx` manifest, which is why a stale catalog is
harmless.

| Field | |
|---|---|
| `id` | reverse-DNS, stable forever, also the directory name |
| `name`, `summary` | what the client lists; summary fits in one PSP line |
| `author` | who publishes the PSP build, not the upstream project |
| `category` | `games`, `emulators`, `apps`, `plugins`, `demos` |
| `license` | SPDX id, or `proprietary` -- mandatory, see docs/open-questions.md |
| `repo` | the project, and by convention where its `app.pspdx` lives |

The manifest is not named. `repo` is a GitHub URL, and the client is pointed at
`raw.githubusercontent.com/<user>/<repo>/HEAD/app.pspdx` -- `HEAD` because the
default branch is `main` in some repositories and `master` in others, and no
entry should have to know which. An entry may still carry a `manifest` of its
own; that is for a file somewhere else in the tree, or a project not on GitHub.

The id comes from a domain the author controls, reversed. Without one, GitHub
supplies it: `io.github.<user>.<app>`, from `github.io`, the same rule Flathub
uses. Stable forever means exactly that -- it is the directory on the Memory
Stick and the key in the install journal, so changing it orphans installs.

Icons and screenshots are a convention, not a field: drop `icon.png` (the
144x80 `ICON0.PNG` out of the EBOOT) or `screenshot.png` (480x272, the size of
the screen) into the directory, and the generated entry gains an `icon` or
`screenshot` path. Leave one out and the entry has none, so the client never
spends a request discovering that there is nothing there.

Both are fetched per app rather than with the catalog, and only for the app the
user is looking at -- a list of fifty must stay one request.
