# PSPDX standard

**Two small JSON files that let a PSP install and update homebrew by itself**

**[→ Read the specification](https://chriopter.github.io/pspdx/)**

PSP homebrew is scattered over GitHub, forums and old archives, and installing or updating it takes a PC. With PSPDX, apps describe themselves in their repositories and catalogs fill in for the rest, so the PSP can find, install and update them directly.

## Overview

Three parts: an **app** is a homebrew, ideally with a `.pspdx` in its repository; a **catalog** is a `catalog.json` that lists many apps; and a **client** such as PSPDX on the PSP reads both.

```mermaid
flowchart RL
  P["PSPDX app<br/>on the PSP"]
  C["catalog.json<br/>apps and their releases"]
  R["App location<br/>with optional .pspdx"]
  P -- browses --> C
  C -- points to --> R
  R -- install and update --> P
```

### How it works

- **`.pspdx`:** the app author puts it next to the app to describe it.
- **Catalog:** provides cached metadata and release info from all listed `.pspdx` files, so update checks on the PSP take one request. If an app has no `.pspdx`, e.g. because it is abandoned or its author doesn't use the standard, the catalog provides the metadata itself.
- **Client:** keeps a copy of each installed app's `.pspdx`, so it can check for and download updates even without a catalog.

## Specification

### `.pspdx`

One app, in the root of its repository.

[Schema](https://chriopter.github.io/pspdx/schema/pspdx-v1.json) · [Fields and rules →](https://chriopter.github.io/pspdx/#pspdx-v1)

<details markdown="1">
<summary><b>Example</b> · a minimal <code>.pspdx</code></summary>

```json
{
  "schema": "https://chriopter.github.io/pspdx/schema/pspdx-v1.json",
  "source": "https://github.com/chriopter/pspdx-demo",
  "name": "PSPDX Demo",
  "category": "demo",
  "summary": "Hello, PSP. A demo listing for PSPDX."
}
```

</details>

### `catalog.json`

A list of many apps with their releases and pictures. For an abandoned app, the entry holds the fields its `.pspdx` would have.

[Schema](https://chriopter.github.io/pspdx/schema/catalog-v1.json) · [Fields and rules →](https://chriopter.github.io/pspdx/#catalog-v1)

## Implementations

- [pspdx-app](https://github.com/chriopter/pspdx-app) — the client for the PSP: browses catalogs, installs release ZIPs, checks for updates at start
- [pspdx-catalog](https://github.com/chriopter/pspdx-catalog) — the main catalog: rebuilds itself every hour, reads each listed repo's `.pspdx` and fetches its latest releases
- [pspdx-demo](https://github.com/chriopter/pspdx-demo) — a minimal homebrew with its own `.pspdx` and a release
- [pspdx-demo-abandoned](https://github.com/chriopter/pspdx-demo-abandoned) — a homebrew without a `.pspdx`, listed by the catalog
