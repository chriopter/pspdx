#!/usr/bin/env python3
"""Forty entries out of the published catalog: each real app repeated under
numbered ids with its own assets copied in, so a long list can be scrolled
without forty real apps. Usage: make-catalog.py <site dir>."""
import json, os, sys, urllib.request

SRC = "https://chriopter.github.io/pspdx-catalog/"
site = sys.argv[1]
os.makedirs(site, exist_ok=True)
cat = json.load(urllib.request.urlopen(SRC + "catalog.json"))
apps = cat["apps"]

def fetch(rel):
    out = os.path.join(site, rel)
    if os.path.exists(out): return
    os.makedirs(os.path.dirname(out), exist_ok=True)
    urllib.request.urlretrieve(SRC + rel, out)

for a in apps:
    for k in ("icon", "screenshot", "video"):
        if a.get(k): fetch(a[k])

out = []
for i in range(40):
    a = dict(apps[i % len(apps)])
    a["id"] = "%s.n%02d" % (a["id"], i)
    a["name"] = "%s %d" % (a["name"], i + 1)
    out.append(a)
cat["apps"] = out
json.dump(cat, open(os.path.join(site, "catalog.json"), "w"))
print("catalog: %d apps" % len(out))
