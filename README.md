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

- **Direct** → a homebrew's repo has a `.pspdx` → add the repo on the PSP → updates from its releases
- **Catalog** → a `catalog.json` links to the repo → add the catalog → updates from the repo's releases
- **Old apps** → the repo has no `.pspdx` → the `catalog.json` carries the details → updates from the catalog

A repo's own `.pspdx` always wins.

## Links

- [pspdx-app](https://github.com/chriopter/pspdx-app) — the PSP app that reads it
- [pspdx-catalog](https://github.com/chriopter/pspdx-catalog) — the main catalog and its builder
- [pspdx-demo](https://github.com/chriopter/pspdx-demo) — Direct and Catalog: a homebrew with its own `.pspdx`
- [pspdx-demo-abandoned](https://github.com/chriopter/pspdx-demo-abandoned) — Old apps: listed without a `.pspdx`
