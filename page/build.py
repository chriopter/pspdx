#!/usr/bin/env python3
"""Builds the Pages site out of README.md and refuses to build a broken one.

The page is the README, written once, laid out as a specification: a header
with the version and the schema URLs, a numbered table of contents, and one
section per file with its fields drawn from the schema by render.js. The
README's "Fields and rules" line and its example move into those sections.

Validators read the schemas from their URLs, so before anything is built
this checks that both schemas are valid, name their own URL as $id, are
linked from the README, and that every example validates. The workflow
checks the live URLs again after the deploy.

    python3 page/build.py [site-dir]     (needs jsonschema and markdown)
"""
import html
import json
import pathlib
import re
import shutil
import subprocess
import sys

import jsonschema
import markdown

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE = "https://chriopter.github.io/pspdx/"
SCHEMAS = {"pspdx-v1.json": ".pspdx", "catalog-v1.json": "catalog.json"}
REPO = "https://github.com/chriopter/pspdx"


def fail(message):
    sys.exit(f"build.py: {message}")


def schemas():
    loaded = {}
    for name in SCHEMAS:
        path = ROOT / "schema" / name
        if not path.is_file():
            fail(f"schema/{name} is missing; validators fetch {BASE}schema/{name}")
        schema = json.loads(path.read_text(encoding="utf-8"))
        jsonschema.Draft202012Validator.check_schema(schema)
        if schema.get("$id") != f"{BASE}schema/{name}":
            fail(f"schema/{name} has $id {schema.get('$id')!r}, not {BASE}schema/{name}")
        loaded[name] = schema
    return loaded


def check_examples(readme, loaded):
    def valid(name, text, where):
        errors = sorted(jsonschema.Draft202012Validator(
            loaded[name], format_checker=jsonschema.FormatChecker()).iter_errors(json.loads(text)), key=str)
        if errors:
            fail(f"{where} breaks {name}: {errors[0].message}")

    blocks = readme.split("```json\n")[1:]
    if not blocks:
        fail("README.md has no json example")
    for block in blocks:
        valid("pspdx-v1.json", block.split("```", 1)[0], "the README's example")
    valid("catalog-v1.json", (ROOT / "page/catalog-example.json").read_text(encoding="utf-8"),
          "page/catalog-example.json")


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", re.sub(r"<[^>]+>", "", text).lower()).strip("-")


def section(name, file, example):
    url = f"{BASE}schema/{name}"
    box = name.removesuffix(".json")
    return (f'<h2 id="{slug(file)}">The <code>{file}</code> file</h2>\n'
            f'<p class="schema-url">Schema <a href="schema/{name}"><code>{url}</code></a></p>\n'
            f'<div class="fields" id="{box}" data-schema="schema/{name}"><p class="dim">Loading the fields…</p></div>\n'
            f'{example}\n')


def updated():
    try:
        out = subprocess.run(["git", "log", "-1", "--format=%cs", "--", "schema"], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout.strip()
        return out or "unknown"
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main():
    site = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "_site")
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    loaded = schemas()
    for name in SCHEMAS:
        if f"({BASE}schema/{name})" not in readme:
            fail(f"README.md no longer links {BASE}schema/{name}")
    check_examples(readme, loaded)

    body = markdown.markdown(readme, extensions=["fenced_code", "md_in_html"])

    title = re.search(r"<h1>(.*?)</h1>\n?", body)
    if not title:
        fail("README.md has no # title")
    body = body.replace(title.group(0), "", 1)
    lede = re.match(r"<p><strong>(.*?)</strong></p>\s*", body.lstrip())
    body = body.lstrip()[lede.end():] if lede else body
    lede = f'<p class="lede">{lede.group(1)}</p>\n' if lede else ""

    # The "Fields and rules" line and the example after it become the sections.
    rules = re.search(r'<p><strong><a href="' + re.escape(BASE) + r'">Fields and rules →</a></strong></p>\s*'
                      r'(<details>.*?</details>)?\s*', body, re.S)
    if not rules:
        fail(f"README.md no longer has the **[Fields and rules →]({BASE})** line the fields go in")
    body = body.replace(rules.group(0), "", 1)
    pspdx_example = rules.group(1) or ""
    catalog = html.escape((ROOT / "page/catalog-example.json").read_text(encoding="utf-8"), quote=False)
    catalog_example = ('<details><summary><b>Example</b> · a <code>catalog.json</code> with one app</summary>'
                       f'<pre><code>{catalog}</code></pre></details>')
    spec = (section("pspdx-v1.json", ".pspdx", pspdx_example) +
            section("catalog-v1.json", "catalog.json", catalog_example))
    if "<h2>Links</h2>" not in body:
        fail("README.md has no ## Links section to put the file sections before")
    body = body.replace("<h2>Links</h2>", spec + "<h2>Links</h2>", 1)

    body = re.sub(r"<h2>(.*?)</h2>", lambda m: f'<h2 id="{slug(m.group(1))}">{m.group(1)}</h2>', body)
    body = re.sub(r'<(h[23]) id="([^"]+)">(.*?)</\1>',
                  lambda m: f'<{m.group(1)} id="{m.group(2)}">{m.group(3)}<a class="anchor" href="#{m.group(2)}" aria-label="Link to this section">#</a></{m.group(1)}>',
                  body)
    body = body.replace(f'href="{BASE}', 'href="')
    toc = "".join(f'<li><a href="#{i}">{re.sub(r"<a class=.anchor.*?</a>", "", t)}</a></li>'
                  for i, t in re.findall(r'<h2 id="([^"]+)">(.*?)</h2>', body))

    urls = "<br>".join(f'<a href="schema/{n}"><code>{BASE}schema/{n}</code></a>' for n in SCHEMAS)
    header = (f'<header>\n<p class="kicker">Specification · Version 1</p>\n<h1>{title.group(1)}</h1>\n{lede}'
              f'<dl class="meta">\n<dt>Version</dt><dd>1 — schema URLs are permanent</dd>\n'
              f'<dt>Schemas</dt><dd>{urls}</dd>\n'
              f'<dt>Updated</dt><dd>{updated()}</dd>\n'
              f'<dt>Source</dt><dd><a href="{REPO}">chriopter/pspdx</a></dd>\n</dl>\n</header>\n')
    nav = f'<nav class="toc" aria-label="Contents"><p>Contents</p><ol>{toc}</ol></nav>\n'
    footer = f'PSPDX standard, version 1 · <a href="{REPO}">source and history</a>'

    page = (ROOT / "page/template.html").read_text(encoding="utf-8")
    page = page.replace("<!-- PAGE -->", header + nav + body).replace("<!-- FOOTER -->", footer)

    if site.exists():
        shutil.rmtree(site)
    (site / "schema").mkdir(parents=True)
    (site / "index.html").write_text(page, encoding="utf-8")
    for name in SCHEMAS:
        shutil.copy(ROOT / "schema" / name, site / "schema" / name)
    shutil.copy(ROOT / "page/render.js", site / "render.js")
    for name in SCHEMAS:
        if f'href="schema/{name}"' not in page:
            fail(f"the page lost its link to schema/{name}")
    print("built", site)


if __name__ == "__main__":
    main()
