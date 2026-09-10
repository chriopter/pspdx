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
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REQUIRED = ("id", "name", "author", "summary", "category", "license", "manifest")


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


def main(out):
    out = Path(out)
    apps = [load(p) for p in sorted((HERE / "apps").glob("*/app.json"))]

    # An icon is optional and never named by hand: an entry gets the field only
    # if the file is there, so the client never spends a request on a 404.
    icons = out.parent / "icons"
    for app in apps:
        src = HERE / "apps" / app["id"] / "icon.png"
        if src.exists():
            icons.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, icons / f"{app['id']}.png")
            app["icon"] = f"icons/{app['id']}.png"
    catalog = {
        "schema": 1,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "apps": apps,
    }
    # Compact separators: the client holds this in RAM, and the PSP has 24 MB.
    text = json.dumps(catalog, ensure_ascii=False, separators=(",", ":"))
    out.write_text(text + "\n", encoding="utf-8")
    withicon = sum("icon" in a for a in apps)
    print(f"{len(apps)} apps ({withicon} with an icon), {len(text)} bytes -> {out}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "catalog.json")
