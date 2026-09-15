# PSPDX standard

**Two small JSON files for PSP homebrew**

PSP homebrew is scattered over GitHub, forums and old archives. With this standard, [PSPDX](https://github.com/chriopter/pspdx-app) on the PSP finds, installs and updates it by itself.

- **`.pspdx`** — one app, in its own repository · [schema](https://chriopter.github.io/pspdx/schema/pspdx-v1.json)
- **`catalog.json`** — many apps in one list, anyone can publish one · [schema](https://chriopter.github.io/pspdx/schema/catalog-v1.json)

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

## How it works

- **Direct:** the repo has a `.pspdx`. Add the repo on the PSP, updates come from its releases.
- **Catalog:** a `catalog.json` links many repos. Add the catalog, each app updates from its repo.
- **Old apps:** the repo has no `.pspdx`. The catalog carries the details and the updates.

A repo's own `.pspdx` always wins.

## Get started

- **Your app:** a `.pspdx` in the repo root and a GitHub release with one ZIP. Only `schema`, `source` and `name` are required.
- **Your catalog:** fork [pspdx-catalog](https://github.com/chriopter/pspdx-catalog) and list your repos.

## Links

- [pspdx-app](https://github.com/chriopter/pspdx-app) — the PSP app
- [pspdx-catalog](https://github.com/chriopter/pspdx-catalog) — the main catalog and its builder
- [pspdx-demo](https://github.com/chriopter/pspdx-demo) — a homebrew with its own `.pspdx`
- [pspdx-demo-abandoned](https://github.com/chriopter/pspdx-demo-abandoned) — a homebrew listed without one
