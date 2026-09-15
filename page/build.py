#!/usr/bin/env python3
"""Builds the Pages site out of README.md and refuses to build a broken one.

The page is the README: one text, written once. Where the README links to
this page ("Fields and rules"), the page opens the fields of both files
instead, drawn from the schemas by render.js.

Validators read the schemas from their URLs, so before anything is built
this checks that both schemas are valid, name their own URL as $id, are
linked from the README, and that every example validates. The workflow
checks the live URLs again after the deploy.

    python3 page/build.py [site-dir]     (needs jsonschema and markdown)
"""
import json
import pathlib
import shutil
import sys

import jsonschema
import markdown

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE = "https://chriopter.github.io/pspdx/"
SCHEMAS = {"pspdx-v1.json": ".pspdx", "catalog-v1.json": "catalog.json"}


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


def fields():
    example = (ROOT / "page/catalog-example.json").read_text(encoding="utf-8")
    out = []
    for name, file in SCHEMAS.items():
        box = name.removesuffix(".json")
        out.append(f'<details><summary><b>Fields and rules</b> · <code>{file}</code></summary>'
                   f'<div id="{box}" data-schema="schema/{name}"><p class="dim">Loading…</p></div></details>')
    esc = example.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    out.append('<details><summary><b>Example</b> · a <code>catalog.json</code> with one app</summary>'
               f'<pre><code>{esc}</code></pre></details>')
    return "\n".join(out)


def main():
    site = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "_site")
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    loaded = schemas()
    for name in SCHEMAS:
        if f"({BASE}schema/{name})" not in readme:
            fail(f"README.md no longer links {BASE}schema/{name}")
    check_examples(readme, loaded)

    body = markdown.markdown(readme, extensions=["fenced_code", "md_in_html"])
    link = f'<p><strong><a href="{BASE}">Fields and rules →</a></strong></p>'
    if link not in body:
        fail("README.md no longer has the **[Fields and rules →](" + BASE + ")** line the fields go in")
    body = body.replace(link, fields()).replace(f'href="{BASE}', 'href="')
    page = (ROOT / "page/template.html").read_text(encoding="utf-8").replace("<!-- README -->", body)

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
