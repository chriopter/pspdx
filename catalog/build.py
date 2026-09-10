#!/usr/bin/env python3
"""Folds catalog/apps/*/app.json into one catalog.json for the client.

One directory per app is what gets edited -- one package per pull request, no
merge conflicts, and a place to put the icon next to the metadata it belongs
to. One file is what gets served -- the PSP pays per handshake, not per byte,
so it must get everything in a single fetch.

No dependencies beyond the standard library, on purpose: this runs in a Pages
workflow and should keep running in ten years.
"""
import json
import re
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REQUIRED = ("id", "name", "author", "summary", "category", "license", "repo")

# Where the manifest lives unless an entry says otherwise. HEAD spares every
# entry from naming a branch, which is main in some repositories and master in
# others; raw serves the tip of the default branch either way.
MANIFEST = "https://raw.githubusercontent.com/{owner}/{repo}/HEAD/app.pspdx"

# The format names itself: a file found on a stick years from now says where
# it came from and which version of the format it is.
SCHEMA = "https://github.com/chriopter/pspdx/blob/master/manifest.md"
GITHUB = re.compile(r"https://github\.com/([^/]+)/([^/]+?)/?$")

# Optional file in an app directory -> where it is served, and the field that
# points at it. Both are 480x272 PNG at most, the size of the screen.
ASSETS = {"icon.png": ("icons", "icon"), "screenshot.png": ("shots", "screenshot")}


def load(path):
    """path is <id>/app.json; the directory name is the id."""
    with path.open(encoding="utf-8") as f:
        app = json.load(f)
    missing = [k for k in REQUIRED if k not in app]
    if missing:
        sys.exit(f"{path.parent.name}: missing {', '.join(missing)}")
    if app["id"] != path.parent.name:
        sys.exit(f"{path.parent.name}: id {app['id']!r} does not match the directory")
    return app


def manifest_url(app):
    """The manifest is app.pspdx in the repository, and normally nothing says
    so. An entry names one only when the file is elsewhere, or the repository
    is not on GitHub."""
    if "manifest" in app:
        return app["manifest"]
    m = GITHUB.match(app["repo"])
    if not m:
        sys.exit(f"{app['id']}: {app['repo']} is not a GitHub repository, "
                 "so the entry has to name its manifest")
    return MANIFEST.format(owner=m.group(1), repo=m.group(2))


def main(out):
    out = Path(out)
    apps = [load(p) for p in sorted((HERE / "apps").glob("*/app.json"))]
    for app in apps:
        app["manifest"] = manifest_url(app)

    # Assets are never named by hand: an entry gets the field only if the file
    # is there, so the client never spends a request discovering a 404.
    for name, (subdir, field) in ASSETS.items():
        for app in apps:
            src = HERE / "apps" / app["id"] / name
            if not src.exists():
                continue
            dest = out.parent / subdir
            dest.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dest / f"{app['id']}.png")
            app[field] = f"{subdir}/{app['id']}.png"
    catalog = {
        "schema": SCHEMA,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "apps": apps,
    }
    # Compact separators: the client holds this in RAM, and the PSP has 24 MB.
    text = json.dumps(catalog, ensure_ascii=False, separators=(",", ":"))
    out.write_text(text + "\n", encoding="utf-8")
    icons = sum("icon" in a for a in apps)
    shots = sum("screenshot" in a for a in apps)
    print(f"{len(apps)} apps, {icons} icons, {shots} screenshots, "
          f"{len(text)} bytes -> {out}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "catalog.json")
