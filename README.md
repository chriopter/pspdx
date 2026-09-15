# PSPDX standard

PSP homebrew is scattered over GitHub, forums and old archives. Installing is
manual, and so is updating.

The PSPDX standard is two small JSON files. They let [PSPDX](https://github.com/chriopter/pspdx-app)
on the PSP itself find, install and update apps.

- **`.pspdx`** describes one app. It lives in the app's own repository, written by the author.
- **`catalog.json`** lists many apps. Anyone can publish one, like an app store.

## How it works

- **Direct:** The author puts a `.pspdx` in the repository. You add the repository
  on the PSP, and PSPDX installs and updates the app from its releases.
- **Catalog:** Someone creates a `catalog.json` that links to many repositories. You
  add the catalog on the PSP, and each app installs and updates from its own repository.
- **Old apps:** The repository has no `.pspdx`, often because the author is gone. The
  `catalog.json` carries the details instead, and updates come through the catalog.

A repository's own `.pspdx` always wins.

## Get started

**Your app.** Put a `.pspdx` in the root of your repository and publish a GitHub
release with one ZIP that holds the app. Only `schema`, `source` and `name` are
required; version, date and size come from the release, icon and pictures from
the EBOOT. Then add the repository on the PSP, or ask a catalog to list it.

**Your catalog.** A `catalog.json` carries every app with its releases: tag, date,
ZIP URL, size and SHA-256. For a repository without a `.pspdx` it supplies the
fields itself. [pspdx-catalog](https://github.com/chriopter/pspdx-catalog) builds
one for you: fork it and list your repositories.

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

**[Fields and rules →](https://chriopter.github.io/pspdx/)**

Each file names its JSON Schema in the `schema` field, so it can be checked:
[`pspdx-v1.json`](https://chriopter.github.io/pspdx/schema/pspdx-v1.json) and
[`catalog-v1.json`](https://chriopter.github.io/pspdx/schema/catalog-v1.json).

## Links

- [pspdx-app](https://github.com/chriopter/pspdx-app) — the PSP app that reads these files
- [pspdx-catalog](https://github.com/chriopter/pspdx-catalog) — the main catalog and its builder
- [pspdx-demo](https://github.com/chriopter/pspdx-demo) — a homebrew with its own `.pspdx`
- [pspdx-demo-abandoned](https://github.com/chriopter/pspdx-demo-abandoned) — a homebrew listed without one
