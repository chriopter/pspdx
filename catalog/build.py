#!/usr/bin/env python3
"""Folds catalog/apps/*.json into one catalog.json for the client.

One file per app is what gets edited -- one package per pull request, no merge
conflicts. One file is what gets served -- the PSP pays per handshake, not per
byte, so it must get everything in a single fetch.

No dependencies beyond the standard library, on purpose: this runs in a Pages
workflow and should keep running in ten years.
"""
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REQUIRED = ("id", "name", "author", "summary", "category", "license", "manifest")


def load(path):
    with path.open(encoding="utf-8") as f:
        app = json.load(f)
    missing = [k for k in REQUIRED if k not in app]
    if missing:
        sys.exit(f"{path.name}: missing {', '.join(missing)}")
    if app["id"] != path.stem:
        sys.exit(f"{path.name}: id {app['id']!r} does not match the file name")
    return app


def main(out):
    apps = [load(p) for p in sorted((HERE / "apps").glob("*.json"))]
    catalog = {
        "schema": 1,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "apps": apps,
    }
    # Compact separators: the client holds this in RAM, and the PSP has 24 MB.
    text = json.dumps(catalog, ensure_ascii=False, separators=(",", ":"))
    Path(out).write_text(text + "\n", encoding="utf-8")
    print(f"{len(apps)} apps, {len(text)} bytes -> {out}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "catalog.json")
