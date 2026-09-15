# PSPDX standard

**Two small JSON files for PSP homebrew**

PSP homebrew is scattered over GitHub, forums and old archives. With this standard, [PSPDX](https://github.com/chriopter/pspdx-app) on the PSP finds, installs and updates it by itself.

- **`.pspdx`** — a breadcrumb in an app's repo that leads the PSP back to it for updates · [schema](https://chriopter.github.io/pspdx/schema/pspdx-v1.json)
- **`catalog.json`** — a list of many homebrews, anyone can publish one · [schema](https://chriopter.github.io/pspdx/schema/catalog-v1.json)

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

- **Direct:** Extreme Tux Racer has a `.pspdx` in its repo. You add the repo on the PSP, PSPDX installs the latest release, and every new release shows up as an update.
- **Catalog:** someone publishes a `catalog.json` with Tux Racer and dozens of other repos. You add that one catalog and browse them all.
- **Old apps:** a homebrew from 2008 has no `.pspdx` and no author left. The catalog writes its name and download itself, and PSPDX installs it from there.

A repo's own `.pspdx` always wins.

**No central server.** The files live with the apps, and anyone can run a catalog. If a catalog disappears, apps with a `.pspdx` keep updating from their author's repo, because PSPDX keeps a copy of each app's `.pspdx` on the Memory Stick.

## Get started

- **Your app:** a `.pspdx` in the repo root and a GitHub release with one ZIP. Only `schema`, `source` and `name` are required. See [pspdx-demo](https://github.com/chriopter/pspdx-demo).
- **Your catalog:** fork [pspdx-catalog](https://github.com/chriopter/pspdx-catalog) and list your repos.

## Links

- [pspdx-app](https://github.com/chriopter/pspdx-app) — the PSP app
- [pspdx-catalog](https://github.com/chriopter/pspdx-catalog) — the main catalog and its builder
- [pspdx-demo](https://github.com/chriopter/pspdx-demo) — a homebrew with its own `.pspdx`
- [pspdx-demo-abandoned](https://github.com/chriopter/pspdx-demo-abandoned) — a homebrew listed without one
