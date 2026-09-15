# PSPDX standard

PSP homebrew is scattered over GitHub, forums and old archives. Installing is manual, and so is updating.

The PSPDX standard solves this with two small JSON files, so [PSPDX](https://github.com/chriopter/pspdx-app) on the PSP itself can find, install and update apps:

- **`.pspdx`** → describes one app, in its own repository · [schema](https://chriopter.github.io/pspdx/schema/pspdx-v1.json)
- **`catalog.json`** → lists many apps, like an app store anyone can run · [schema](https://chriopter.github.io/pspdx/schema/catalog-v1.json)

**[Fields and rules →](https://chriopter.github.io/pspdx/)**

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

## How apps get onto the PSP and stay updated

- **Direct:** The author puts a `.pspdx` in the homebrew's repository. You add the repository on the PSP, and PSPDX installs and updates the app from its releases.
- **Catalog:** Someone creates a `catalog.json` that links to many repositories. You add the catalog on the PSP, and each app installs and updates from its own repository.
- **Old apps:** The repository has no `.pspdx`, often because the author is gone. The `catalog.json` carries the details instead, and updates come through the catalog.

A repository's own `.pspdx` always wins.

## Links

- [pspdx-app](https://github.com/chriopter/pspdx-app) — the PSP app that reads it
- [pspdx-catalog](https://github.com/chriopter/pspdx-catalog) — the main catalog and its builder
- [pspdx-demo](https://github.com/chriopter/pspdx-demo) — Direct and Catalog: a homebrew with its own `.pspdx`
- [pspdx-demo-abandoned](https://github.com/chriopter/pspdx-demo-abandoned) — Old apps: listed without a `.pspdx`
