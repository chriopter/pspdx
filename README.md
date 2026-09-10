# PSPDX

**PSP Download Index** — the homebrew package index for the PlayStation Portable.

> The PSP model Sony never shipped.

The client fetches a catalog, installs a package and checks for updates. It
runs under PPSSPP; nobody has put it on real hardware yet.

| | |
|---|---|
| [`app/`](app/) | the on-device client: TLS 1.3, entropy off the analog stick, install |
| [`catalog/`](catalog/) | one file per app, folded into the `catalog.json` the client fetches |
| [`docs/`](docs/) | the design notes, including the approaches that were dropped and why |

Updates never go through the catalog. Each package's `.pspdx` manifest lives
in its author's own repository and is the only thing compared, which is why a
stale catalog costs nothing.
