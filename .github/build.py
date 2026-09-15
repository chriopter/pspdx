#!/usr/bin/env python3
"""Builds the Pages site out of README.md and refuses to build a broken one.

The page is the README, written once, laid out as a specification: a header
with the version and the schema URLs and a numbered table of contents.
Each README "Fields and rules" line becomes that file's fields, folded and
drawn from the schema by render.js. The page shell and render.js live below
as TEMPLATE and RENDER, so the whole build is this one file.

Validators read the schemas from their URLs, so before anything is built
this checks that both schemas are valid, name their own URL as $id, are
linked from the README, and that every json example in the README validates
against the schema its "schema" field names. The workflow checks the live
URLs again after the deploy.

    python3 .github/build.py [site-dir]     (needs jsonschema and markdown)
"""
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
    """Every ```json block names its schema and validates against it; each schema has one."""
    seen = set()
    for n, text in enumerate(re.findall(r"^```json\n(.*?)^```", readme, re.S | re.M), 1):
        where = f"README.md json example {n}"
        try:
            example = json.loads(text)
        except json.JSONDecodeError as e:
            fail(f"{where} is not JSON: {e}")
        url = example.get("schema") if isinstance(example, dict) else None
        name = url.removeprefix(f"{BASE}schema/") if isinstance(url, str) and url.startswith(f"{BASE}schema/") else None
        if name not in loaded:
            fail(f"{where} has schema {url!r}, not one of " + ", ".join(f"{BASE}schema/{s}" for s in SCHEMAS))
        errors = sorted(jsonschema.Draft202012Validator(
            loaded[name], format_checker=jsonschema.FormatChecker()).iter_errors(example), key=str)
        if errors:
            fail(f"{where} breaks {name}: {errors[0].message}")
        seen.add(name)
    for name, file in SCHEMAS.items():
        if name not in seen:
            fail(f"README.md has no json example of a {file} ({name})")


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", re.sub(r"<[^>]+>", "", text).lower()).strip("-")


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
    # The README's link to this page is the page itself here.
    body = re.sub(r'<p><strong><a href="' + re.escape(BASE) + r'">[^<]*</a></strong></p>\s*', "", body, count=1)
    lede = re.match(r"<p><strong>(.*?)</strong></p>\s*", body.lstrip())
    body = body.lstrip()[lede.end():] if lede else body
    lede = f'<p class="lede">{lede.group(1)}</p>\n' if lede else ""

    # Each "Fields and rules" line opens that file's fields, folded, in place.
    for name, file in SCHEMAS.items():
        box = name.removesuffix(".json")
        link = f' · <a href="{BASE}#{box}">Fields and rules →</a>'
        para = re.search(r"<p>[^\n]*?" + re.escape(link) + r"</p>", body)
        if not para:
            fail(f"README.md no longer has a line ending in · [Fields and rules →]({BASE}#{box}) for {file}")
        fold = (f'<details><summary><b>Fields and rules</b> · <code>{file}</code></summary>'
                f'<div class="fields" id="{box}" data-schema="schema/{name}"><p class="dim">Loading the fields…</p></div></details>')
        body = body.replace(para.group(0), para.group(0).replace(link, "") + "\n" + fold, 1)

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

    first = body.find("<h2")
    page = TEMPLATE.replace("<!-- PAGE -->", header + body[:first] + nav + body[first:]).replace("<!-- FOOTER -->", footer)

    if site.exists():
        shutil.rmtree(site)
    (site / "schema").mkdir(parents=True)
    (site / "index.html").write_text(page, encoding="utf-8")
    for name in SCHEMAS:
        shutil.copy(ROOT / "schema" / name, site / "schema" / name)
    (site / "render.js").write_text(RENDER, encoding="utf-8")
    for name in SCHEMAS:
        if f'href="schema/{name}"' not in page:
            fail(f"the page lost its link to schema/{name}")
    print("built", site)


# The page shell: the README and the two schemas go where the comment in <main> is.
TEMPLATE = r'''<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PSPDX standard</title>
<meta name="description" content="PSPDX v1: two small JSON files that let a PSP find, install and update homebrew.">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:ital,wght@0,400;0,600;1,400&display=swap">
<!-- Built by .github/build.py: the README and the two schemas go where the comment in <main> is. -->
<style>
  :root {
    --ground: #f6f7f9; --paper: #ffffff; --ink: #151a21; --dim: #5a6372; --rule: #dde2ea;
    --code: #eef1f5; --accent: #2346b8;
    --sans: "IBM Plex Sans", system-ui, -apple-system, "Segoe UI", sans-serif;
    --mono: "IBM Plex Mono", ui-monospace, Menlo, Consolas, monospace;
  }
  @media (prefers-color-scheme: dark) {
    :root { --ground: #0c0f14; --paper: #12161d; --ink: #e4e7ec; --dim: #99a2b0; --rule: #262d38; --code: #1b212b; --accent: #8aa8ff; }
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--ground); color: var(--ink); font: 16.5px/1.7 var(--sans); padding-inline: 16px; }
  main { max-width: 880px; margin: 2.5rem auto 5rem; background: var(--paper); border: 1px solid var(--rule); border-radius: 8px; padding: 2.75rem clamp(1.1rem, 5vw, 3.5rem) 3.5rem; counter-reset: section; }
  header { border-bottom: 1px solid var(--rule); padding-bottom: 1.5rem; margin-bottom: 1.75rem; }
  .kicker { font: 500 .78rem/1 var(--mono); letter-spacing: .08em; text-transform: uppercase; color: var(--accent); margin: 0 0 .7rem; }
  h1 { font-size: 2.2rem; line-height: 1.15; margin: 0 0 1.25rem; font-weight: 600; text-wrap: balance; }
  dl.meta { display: grid; grid-template-columns: max-content 1fr; gap: .3rem 1.25rem; margin: 0; font-size: .9rem; }
  dl.meta dt { color: var(--dim); }
  dl.meta dd { margin: 0; min-width: 0; overflow-wrap: anywhere; }
  nav.toc { margin: 2rem 0; font-size: .92rem; }
  nav.toc p { margin: 0 0 .3rem; color: var(--dim); font: 500 .75rem/1 var(--mono); letter-spacing: .08em; text-transform: uppercase; }
  nav.toc ol { margin: 0; padding-left: 1.4rem; }
  nav.toc li { margin: .1rem 0; }
  h2 { counter-increment: section; font-size: 1.35rem; line-height: 1.3; margin: 3rem 0 .9rem; padding-top: 1.25rem; border-top: 1px solid var(--rule); font-weight: 600; text-wrap: balance; }
  h2::before { content: counter(section) "."; color: var(--accent); font: 500 1.05rem var(--mono); margin-right: .6rem; }
  h3 { font-size: 1.08rem; margin: 1.75rem 0 .4rem; font-weight: 600; }
  h2 + h3 { margin-top: .5rem; }
  .fields h3 { margin: 1.5rem 0 .4rem; }
  h2 a.anchor, h3 a.anchor { color: var(--dim); text-decoration: none; margin-left: .4rem; opacity: 0; font-weight: 400; }
  h2:hover a.anchor, h3:hover a.anchor, a.anchor:focus-visible { opacity: 1; }
  p, li { max-width: 72ch; }
  p { margin: 0 0 1rem; }
  a { color: var(--accent); text-underline-offset: 2px; }
  a:focus-visible, summary:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; border-radius: 3px; }
  code, pre { font-family: var(--mono); font-size: .87em; }
  :not(pre) > code { background: var(--code); padding: .08em .35em; border-radius: 4px; }
  pre { background: var(--code); padding: 1rem 1.1rem; border-radius: 6px; overflow-x: auto; margin: .75rem 0 1.25rem; line-height: 1.55; }
  ul, ol { padding-left: 1.3rem; margin: 0 0 1rem; }
  li + li { margin-top: .35rem; }
  .lede { font-size: 1.15rem; margin: -.4rem 0 1.25rem; color: var(--dim); }
  .schema-url { font-size: .9rem; margin: -.25rem 0 1.25rem; color: var(--dim); }
  details { margin: .75rem 0; }
  summary { cursor: pointer; list-style: none; }
  summary::-webkit-details-marker { display: none; }
  summary::before { content: "▸"; display: inline-block; width: 1.1em; color: var(--dim); }
  details[open] > summary::before { content: "▾"; }
  .dim { color: var(--dim); }
  .fields { font-size: .9rem; }
  .fields h3 { font-size: .98rem; }
  .scroll { overflow-x: auto; margin: 0 0 1rem; }
  table { border-collapse: collapse; width: 100%; min-width: 36rem; }
  th, td { text-align: left; vertical-align: top; padding: .5rem .55rem; border-bottom: 1px solid var(--rule); }
  th { color: var(--dim); font-weight: 600; font-size: .82rem; }
  th:nth-child(1) { width: 15%; }
  th:nth-child(2) { width: 12%; }
  th:nth-child(3) { width: 33%; }
  td.name { white-space: nowrap; }
  td code { overflow-wrap: anywhere; }
  details.pattern { margin: .15rem 0 0; }
  details.pattern summary { color: var(--dim); }
  details.pattern code { display: block; margin-top: .25rem; white-space: pre-wrap; word-break: break-all; }
  .req { font-weight: 600; }
  ul.facts, ul.rules { padding-left: 1rem; margin: 0; }
  ul.facts li, ul.rules li { margin: .1rem 0; }
  .diagram { margin: 1.5rem 0 .5rem; text-align: center; overflow-x: auto; }
  .diagram svg { max-width: 100%; height: auto; }
  footer { max-width: 880px; margin: -3.5rem auto 3rem; padding: 0 .5rem; font-size: .82rem; color: var(--dim); }
</style>
<main>
<!-- PAGE -->
</main>
<footer><!-- FOOTER --></footer>
<script src="render.js"></script>
<script type="module">
  // The README's diagrams are mermaid blocks, which GitHub draws itself; here they are drawn the same way.
  const blocks = [...document.querySelectorAll("pre > code.language-mermaid")];
  if (blocks.length) {
    const { default: mermaid } = await import("https://cdn.jsdelivr.net/npm/mermaid@11.4.1/dist/mermaid.esm.min.mjs");
    const dark = matchMedia("(prefers-color-scheme: dark)").matches;
    mermaid.initialize({ startOnLoad: false, theme: dark ? "dark" : "neutral", fontFamily: "IBM Plex Sans, system-ui, sans-serif" });
    for (const code of blocks) {
      const div = document.createElement("div");
      div.className = "diagram";
      div.textContent = code.textContent;
      code.parentElement.replaceWith(div);
    }
    await mermaid.run({ querySelector: ".diagram" });
  }
</script>
</html>
'''

# render.js, written beside index.html: each schema drawn as tables of its fields.
RENDER = r'''// Reads a schema and writes it out for people: every object as a table of its
// fields, with what is required, what values are allowed and what the rules
// between fields say. Nothing here is written by hand, so the page cannot
// drift from the file.
//
// Every element with data-schema="<url>" is filled with its schema; the url is
// relative to the page, and the element's id prefixes the anchors inside it.
(() => {
const esc = s => String(s).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const code = v => `<code>${esc(typeof v === "string" ? v : JSON.stringify(v))}</code>`;
const list = a => a.map(code).join(", ");

// The patterns that only keep out control characters or ask for https say
// so in words; any other pattern is shown as it is written.
const SAID = {
  "^[^\\u0000-\\u001f]*(?![\\s\\S])": "no control characters",
  "^[^\\u0000-\\u0009\\u000b-\\u001f]*(?![\\s\\S])": "no control characters except a newline",
  "^https://[^\\u0000-\\u001f]+(?![\\s\\S])": "an https:// address",
  "^https://": "an https:// address",
  "^[0-9a-f]{64}(?![\\s\\S])": "64 lowercase hex digits",
  "^[0-9a-f]{32}(?![\\s\\S])": "32 lowercase hex digits",
};
// A long pattern would stretch its column; it is folded under a short label.
const pat = pattern => pattern.length <= 32 ? code(pattern)
  : `<details class="pattern"><summary>a pattern</summary>${code(pattern)}</details>`;
const said = pattern => SAID[pattern] || `matches ${pat(pattern)}`;
const strip = desc => (desc || "").replace(/^(required|optional);\s*/i, "");

function typeOf(p, pre) {
  if (p.$ref) { const d = p.$ref.split("/").pop(); return `object <a href="#${esc(pre)}-${esc(d)}">${esc(d)}</a>`; }
  if (p.const !== undefined) return "fixed value";
  if (p.enum) return "one of";
  if (p.type === "array") return `list of ${p.items ? typeOf(p.items, pre) : "values"}`;
  if (p.type === "object" && p.properties) return "object, below";
  return esc(p.type || (p.anyOf ? "string" : "any"));
}

function facts(p) {
  const f = [];
  if (p.const !== undefined) f.push(`exactly ${code(p.const)}`);
  if (p.enum) f.push(list(p.enum));
  if (p.default !== undefined) f.push(`default ${code(p.default)}`);
  const len = (lo, hi, unit) => {
    if (lo !== undefined && hi !== undefined) return lo === hi ? `${lo} ${unit}` : `${lo}–${hi} ${unit}`;
    if (hi !== undefined) return `up to ${hi} ${unit}`;
    if (lo !== undefined) return `at least ${lo} ${unit}`;
  };
  const l = len(p.minLength, p.maxLength, "characters"); if (l) f.push(l);
  const n = len(p.minItems, p.maxItems, "entries"); if (n) f.push(n);
  if (p.uniqueItems) f.push("no duplicates");
  if (p.minimum !== undefined) f.push(`at least ${p.minimum}`);
  if (p.format) f.push(`format ${code(p.format)}`);
  if (p.pattern) f.push(said(p.pattern));
  if (p.anyOf) f.push("either " + p.anyOf.map(a => [a.format && `format ${code(a.format)}`, a.pattern && `matching ${pat(a.pattern)}`].filter(Boolean).join(" ")).join("<br>or "));
  if (p.if && p.then) f.push(`if it ${cond(p.if)}, then it also ${cond(p.then)}`);
  if (p.items && p.items.type !== "object" && !p.items.$ref) {
    const inner = facts(p.items); if (inner.length) f.push("each: " + inner.join("; "));
  }
  return f;
}

// The few shapes of condition the PSPDX schemas use, said in words; anything
// else is shown as JSON rather than guessed at.
function cond(c) {
  const parts = [];
  if (c.pattern) parts.push(`matches ${pat(c.pattern)}`);
  if (c.not && c.not.pattern) parts.push(`does not match ${pat(c.not.pattern)}`);
  if (c.required) parts.push(`has ${list(c.required)}`);
  if (c.not && c.not.required) parts.push(`has no ${list(c.not.required)}`);
  if (c.anyOf) parts.push(c.anyOf.map(cond).join(" or "));
  if (c.properties) for (const [k, v] of Object.entries(c.properties)) {
    if (v.enum) parts.push(`${code(k)} is one of ${list(v.enum)}`);
    else if (v.const !== undefined) parts.push(`${code(k)} is ${code(v.const)}`);
    else if (v.not && v.not.pattern) parts.push(`${code(k)} does not match ${pat(v.not.pattern)}`);
    else if (v.pattern) parts.push(`${code(k)} matches ${pat(v.pattern)}`);
    else if (v.required) parts.push(`${code(k)} has ${list(v.required)}`);
  }
  return parts.length ? parts.join(" and ") : code(c);
}

// A condition about the entry itself reads "it has …"; one about a field of
// it already names the field, so it reads "its `release` has …".
function subject(text) { return (text.startsWith("<code>") ? "its " : "it ") + text; }

function rules(obj) {
  return (obj.allOf || []).map(r => r.if
    ? `<li>If the entry ${cond(r.if)}, then ${subject(cond(r.then))}${r.else ? `; otherwise ${subject(cond(r.else))}` : ""}.</li>`
    : `<li>${code(r)}</li>`).join("");
}

function table(pre, id, obj, heading) {
  const req = new Set(obj.required || []);
  let html = `<h3 id="${esc(pre)}-${esc(id)}">${heading}</h3>`;
  if (obj.description) html += `<p class="dim">${esc(strip(obj.description))}</p>`;
  html += `<div class="scroll"><table><tr><th>Field</th><th>Type</th><th>Allowed</th><th>Meaning</th></tr>`;
  const nested = [];
  for (const [k, p] of Object.entries(obj.properties || {})) {
    const f = facts(p);
    html += `<tr><td class="name">${code(k)}<br>${req.has(k) ? '<span class="req">required</span>' : '<span class="dim">optional</span>'}</td>` +
            `<td>${typeOf(p, pre)}</td><td>${f.length ? `<ul class="facts">${f.map(x => `<li>${x}</li>`).join("")}</ul>` : ""}</td>` +
            `<td>${esc(strip(p.description))}</td></tr>`;
    if (p.type === "object" && p.properties) nested.push([`${id}-${k}`, p, `${code(k)} in ${esc(heading.replace(/<[^>]+>/g, ""))}`]);
  }
  html += `</table></div>`;
  const r = rules(obj); if (r) html += `<p><strong>Rules</strong></p><ul class="rules">${r}</ul>`;
  html += `<p class="dim">${obj.additionalProperties === false ? "Fields not listed here are rejected." : "Fields not listed here are allowed and ignored."}</p>`;
  for (const [nid, p, h] of nested) html += table(pre, nid, p, h);
  return html;
}

function render(s, pre) {
  let html = table(pre, "top", s, "Top level");
  for (const [k, d] of Object.entries(s.$defs || {})) if (d.properties) html += table(pre, k, d, code(k));
  return html;
}

// A link to an anchor inside a closed section opens the section first.
function reveal() {
  const el = location.hash.length > 1 && document.getElementById(decodeURIComponent(location.hash.slice(1)));
  if (!el) return;
  for (let d = el.closest("details"); d; d = d.parentElement.closest("details")) d.open = true;
  el.scrollIntoView();
}
addEventListener("hashchange", reveal);

Promise.all([...document.querySelectorAll("[data-schema]")].map(box => {
  const url = box.dataset.schema;
  return fetch(url).then(r => { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(s => { box.innerHTML = render(s, box.id || "s"); })
    .catch(e => { box.innerHTML = `<p>Could not read ${code(url)} (${esc(e.message)}).</p>`; });
})).then(reveal);
})();
'''

if __name__ == "__main__":
    main()
