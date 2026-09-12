#!/usr/bin/env python3
"""Writes the release half of app.pspdx, so that nobody has to type a sha256.

Runs in the author's repository, either on `release: published` or, given a
tag, as the step after the workflow's own `gh release create` (a release
made with GITHUB_TOKEN fires no event); README.md shows both. And on a
maintainer's desk for a repository whose workflow is not wired up yet:

    release.py --repo owner/name --tag v1.2.3 --write-only
    release.py --repo owner/name --write-only            the newest release

Either way it picks the zip on the release, downloads it, hashes the bytes
it actually got, looks inside for the EBOOT, and writes `version`, `rev`,
`url`, `sha256`, `size` and `root` into app.pspdx under the author's half.
The author's half it only reads. When there is none it drafts one from what
GitHub knows about the repository and says so, and either way the half is
held to the rules in manifest.md before anything is written.

In the workflow it then commits the file to the default branch and attaches
a copy to the release. With --write-only it stops at the file in the current
directory, for a person to look at and commit.

No dependencies beyond the standard library: this runs in a workflow and
should keep running in ten years.
"""
import fnmatch
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
import zipfile
from datetime import datetime
from pathlib import Path

FILE = "app.pspdx"
SCHEMA = "https://github.com/chriopter/pspdx/blob/master/manifest.md"
AUTHOR = ("id", "name", "author", "summary", "category", "license", "repo")
RELEASE = ("version", "rev", "url", "sha256", "size", "root")
CATEGORIES = ("games", "emulators", "apps", "plugins", "demos")
GITHUB = re.compile(r"https://github\.com/([^/]+)/([^/]+?)(?:\.git)?/?$", re.I)

# A release with several assets has to be told apart; one .zip needs no rule.
DEFAULT_ASSET = "*.zip"

# The whole archive is held in memory to hash it and read its index.
# GitHub allows 2 GB assets; a PSP package that size is a mistake.
MAX_ASSET = 256 * 1024 * 1024


def _token():
    """The workflow's token, or gh's on a desk. Unauthenticated works for a
    --write-only run, just 60 calls an hour."""
    if os.environ.get("GITHUB_TOKEN"):
        return os.environ["GITHUB_TOKEN"]
    try:
        out = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True)
        return out.stdout.strip() or None
    except FileNotFoundError:
        return None


TOKEN = _token()


def api(path, method="GET", body=None, ctype="application/json"):
    """One call. Returns the JSON, or None when GitHub answers with nothing,
    which a DELETE does."""
    url = path if path.startswith("https://") else "https://api.github.com" + path
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "pspdx-release")
    if body is not None:
        req.add_header("Content-Type", ctype)
    if TOKEN:
        req.add_header("Authorization", "Bearer " + TOKEN)
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            raw = r.read()
            return json.loads(raw) if raw else None
    except urllib.error.HTTPError as e:
        if e.code == 404 and path.endswith("/releases/latest"):
            raise SystemExit("no releases yet") from None
        if e.code == 404 and "/releases/tags/" in path:
            raise SystemExit("no release with that tag") from None
        if e.code == 404:
            raise SystemExit("no such repository, or the token cannot see it") from None
        if e.code == 403 and e.headers.get("x-ratelimit-remaining") == "0":
            raise SystemExit("rate limited; set GITHUB_TOKEN") from None
        detail = e.read().decode(errors="replace").strip()[:300]
        raise SystemExit(f"GitHub said {e.code} for {method} {path}\n  {detail}") from None
    except urllib.error.URLError as e:
        raise SystemExit(f"cannot reach GitHub: {e.reason}") from None


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "pspdx-release"})
    with urllib.request.urlopen(req, timeout=300) as r:
        return r.read()


def pick_asset(release, pattern):
    names = [a for a in release.get("assets", [])
             if fnmatch.fnmatch(a["name"].lower(), pattern.lower())]
    if len(names) == 1:
        return names[0]
    have = [a["name"] for a in release.get("assets", [])]
    if not have:
        raise SystemExit(f"{release['tag_name']} has no assets attached")
    if not names:
        raise SystemExit(f"no asset matches {pattern!r}; release has "
                         + ", ".join(have) + f"\n  set \"asset\" in {FILE}")
    raise SystemExit(f"{len(names)} assets match {pattern!r}: "
                     + ", ".join(a["name"] for a in names)
                     + f"\n  narrow \"asset\" in {FILE}")


def eboot_root(raw):
    """Where the package sits inside the archive.

    The shallowest EBOOT.PBP wins and its directory is the package: of
    sixteen surveyed archives that hold an EBOOT, ten put it one directory
    down, three at the root and only two under PSP/GAME/. An empty root means
    the archive is the package.
    """
    try:
        z = zipfile.ZipFile(io.BytesIO(raw))
    except zipfile.BadZipFile:
        raise SystemExit("the asset is not a zip") from None
    # A zip written on Windows carries backslashes, and without this the depth
    # test sees one component and calls every layout the root.
    ebs = [n.replace("\\", "/") for n in z.namelist()
           if n.lower().endswith("eboot.pbp")]
    if not ebs:
        raise SystemExit("no EBOOT.PBP in the archive")
    key = lambda n: (n.count("/"), len(n))
    ebs.sort(key=key)
    tied = [n for n in ebs if key(n) == key(ebs[0])]
    if len(tied) > 1:
        raise SystemExit("two EBOOT.PBP at the same depth: "
                         + ", ".join(tied) + "\n  one archive is one package")
    path = ebs[0]
    if path.startswith("/") or ".." in path.split("/"):
        raise SystemExit(f"the archive puts its EBOOT at {path!r}")
    return path.rsplit("/", 1)[0] + "/" if "/" in path else ""


def clean(part):
    return re.sub(r"[^a-z0-9]", "", part.lower())


def trim(text, limit=60):
    """A summary cut mid-word reads like a bug. Cut on a space instead."""
    text = " ".join(text.split())
    if len(text) <= limit:
        return text
    return text[:limit].rsplit(" ", 1)[0].rstrip(",.;:") + "..."


def draft(owner, repo):
    """A first author's half, from what the repository says about itself.
    Every field of it is a guess to be corrected by hand, the id included:
    its owner part is fixed, its name part is the repository's name with
    the dashes dropped, which is the default and not necessarily the app."""
    meta = api(f"/repos/{owner}/{repo}")
    lic = (meta.get("license") or {}).get("spdx_id")
    return {
        "id": "io.github.{}.{}".format(clean(owner), clean(repo)),
        "name": repo.replace("-", " ").replace("_", " ").title(),
        "author": owner,
        "summary": trim(meta.get("description") or ""),
        "category": "apps",
        "license": lic if lic and lic != "NOASSERTION" else "",
        "repo": f"https://github.com/{owner}/{repo}",
    }


def load(text):
    """The author's half out of the file that is there: everything that is
    not the schema and not ours, in the order they wrote it, so that a field
    the format does not know about survives the rewrite."""
    try:
        data = json.loads(text)
    except json.JSONDecodeError as e:
        raise SystemExit(f"{FILE} is not JSON: {e}") from None
    if not isinstance(data, dict):
        raise SystemExit(f"{FILE} is not a JSON object")
    return {k: v for k, v in data.items() if k != "schema" and k not in RELEASE}


def check(author, owner, repo):
    """The rules in manifest.md, each as a sentence a person can act on."""
    bad = []
    for k in AUTHOR:
        if not isinstance(author.get(k), str):
            bad.append(f'"{k}" is missing or not a string')
    if bad:
        return bad
    # The owner part is checked and the name part is not: the owner is what
    # stops a file from claiming another account's app, and the name is the
    # author's to choose, since the app is often not called what its
    # repository is.
    prefix = f"io.github.{clean(owner)}."
    if not re.fullmatch(re.escape(prefix) + r"[a-z0-9]+", author["id"]):
        bad.append(f'"id" is {author["id"]!r}; it has to be {prefix}<name>, the name '
                   'lower case letters and digits')
    if len(author["id"]) > 80:
        bad.append(f'"id" is {len(author["id"])} characters; at most 80')
    if not 0 < len(author["name"]) < 40:
        bad.append(f'"name" is {len(author["name"])} characters; under 40, or the row clips it')
    s = author["summary"]
    if "\n" in s or len(s) > 60:
        bad.append(f'"summary" is {len(s)} characters; one line of at most 60')
    if author["category"] not in CATEGORIES:
        bad.append(f'"category" is {author["category"]!r}; one of ' + ", ".join(CATEGORIES))
    m = GITHUB.match(author["repo"])
    if not m or (m.group(1).lower(), m.group(2).lower()) != (owner.lower(), repo.lower()):
        bad.append(f'"repo" is {author["repo"]!r}; this file lives in '
                   f'https://github.com/{owner}/{repo}')
    if "asset" in author and not isinstance(author["asset"], str):
        bad.append('"asset" is not a string; it is a glob like "*-psp.zip"')
    return bad


def render(author, release):
    """The file as manifest.md shows it: the author's half, a blank line,
    the release half, values in a column. json.dumps would do, but a file
    people edit by hand should look like the one in the manifest."""
    top = {"schema": SCHEMA, **author}
    width = max(len(k) for k in [*top, *release]) + 3         # quotes and colon

    def lines(d):
        return [f'  {json.dumps(k) + ":":<{width}} {json.dumps(v, ensure_ascii=False)}'
                for k, v in d.items()]
    return ("{\n" + ",\n".join(lines(top)) + ",\n\n"
            + ",\n".join(lines(release)) + "\n}\n")


def version_of(tag):
    """The tag without its v. Only a v in front of a digit is the prefix;
    a tag called "version" keeps its letters."""
    return tag[1:] if tag[:1] in "vV" and tag[1:2].isdigit() else tag


def release_half(rel, asset, raw):
    published = rel["published_at"].replace("Z", "+00:00")
    return {
        "version": version_of(rel["tag_name"]),
        "rev": int(datetime.fromisoformat(published).timestamp()),
        "url": asset["browser_download_url"],
        "sha256": hashlib.sha256(raw).hexdigest(),
        "size": len(raw),
        "root": eboot_root(raw),
    }


def git(*args, cwd):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"git {' '.join(args)} failed:\n  {r.stderr.strip()}")
    return r.stdout


def checkout(cwd, branch):
    """The workflow checks out the tag, detached. The file belongs on the
    default branch, which is what raw.githubusercontent.com/HEAD serves the
    console, so that branch is what is read and what is written. The
    checkout is shallow more often than not, hence the depth."""
    git("fetch", "--depth=1", "origin", branch, cwd=cwd)
    git("checkout", "-q", "-B", branch, "FETCH_HEAD", cwd=cwd)


def commit(cwd, branch, version):
    git("add", FILE, cwd=cwd)
    git("-c", "user.name=pspdx-release", "-c", "user.email=noreply@github.com",
        "commit", "-q", "-m", f"{FILE} for {version}", cwd=cwd)
    try:
        git("push", "-q", "origin", f"HEAD:refs/heads/{branch}", cwd=cwd)
    except SystemExit as e:
        raise SystemExit(f"{e}\n  the workflow needs `permissions: contents: write`, "
                         "and actions/checkout must keep its credentials") from None


def upload(owner, repo, rel, text):
    """A copy on the release itself, replacing the one from an earlier run."""
    for a in api(f"/repos/{owner}/{repo}/releases/{rel['id']}/assets") or []:
        if a["name"] == FILE:
            api(f"/repos/{owner}/{repo}/releases/assets/{a['id']}", method="DELETE")
    api(f"https://uploads.github.com/repos/{owner}/{repo}/releases/{rel['id']}/assets"
        f"?name={FILE}", method="POST", body=text.encode())


def parse(argv):
    opts = {"repo": None, "tag": None, "asset": None, "write_only": False}
    it = iter(argv)
    for a in it:
        if a == "--write-only":
            opts["write_only"] = True
        elif a in ("--repo", "--tag", "--asset"):
            opts[a[2:]] = next(it, None)
            if not opts[a[2:]]:
                raise SystemExit(f"{a} needs a value\n{__doc__}")
        elif a in ("-h", "--help"):
            raise SystemExit(__doc__)
        else:
            raise SystemExit(f"unknown argument {a!r}\n{__doc__}")
    return opts


def main(argv):
    o = parse(argv)
    if o["repo"] and not o["write_only"]:
        raise SystemExit("on a desk this only writes: add --write-only")
    if o["write_only"]:
        if not o["repo"] or o["repo"].count("/") != 1:
            raise SystemExit("--repo owner/name is needed with --write-only")
        owner, repo = o["repo"].split("/")
        rel = api(f"/repos/{owner}/{repo}/releases/tags/{o['tag']}" if o["tag"]
                  else f"/repos/{owner}/{repo}/releases/latest")
        cwd, branch = Path.cwd(), None
    elif o["tag"] or os.environ.get("PSPDX_TAG"):
        # A release that a workflow itself created with GITHUB_TOKEN fires no
        # `release` event, so nothing would ever run on it. Given the tag,
        # this runs as the step after `gh release create` instead, with the
        # release fetched from the API and the repository named by the runner.
        full = os.environ.get("GITHUB_REPOSITORY") or ""
        if full.count("/") != 1:
            raise SystemExit("not in a workflow; on a desk use --repo owner/name --write-only")
        owner, repo = full.split("/")
        tag = o["tag"] or os.environ["PSPDX_TAG"]
        rel = api(f"/repos/{owner}/{repo}/releases/tags/{tag}")
        branch = api(f"/repos/{owner}/{repo}")["default_branch"]
        cwd = Path(os.environ.get("GITHUB_WORKSPACE") or ".")
    else:
        path = os.environ.get("GITHUB_EVENT_PATH")
        if not path:
            raise SystemExit("not in a workflow; on a desk use --repo owner/name --write-only")
        event = json.loads(Path(path).read_text())
        rel = event.get("release")
        if not rel:
            raise SystemExit("not a release event and no tag given; the workflow wants "
                             "`on: release: types: [published]`, or `with: tag:`")
        meta = event["repository"]
        owner, repo, branch = meta["owner"]["login"], meta["name"], meta["default_branch"]
        cwd = Path(os.environ.get("GITHUB_WORKSPACE") or ".")

    if branch:
        # The console takes the highest rev it sees, so a prerelease written
        # here would be what every console installs; releases/latest never
        # pointed at one either. `published` fires for prereleases too.
        if rel.get("prerelease"):
            print(f"{rel['tag_name']} is a prerelease; {FILE} stays on the last release")
            return 0
        if rel.get("draft"):
            raise SystemExit(f"{rel['tag_name']} is still a draft; publish it first")
        checkout(cwd, branch)

    tag = rel["tag_name"]
    print(f"{owner}/{repo} {tag}")

    path = cwd / FILE
    before = path.read_text() if path.exists() else None
    if before is None:
        author = draft(owner, repo)
        print(f"  no {FILE}; drafted the author's half from the repository. "
              "Read it before trusting it.")
    else:
        author = load(before)
    problems = check(author, owner, repo)
    if problems:
        raise SystemExit(f"{FILE} does not follow the format:\n"
                         + "\n".join("  " + p for p in problems)
                         + f"\n  the rules are at {SCHEMA}")
    if not author.get("license"):
        print('  "license" is empty; fill in an SPDX identifier')

    pattern = o["asset"] or os.environ.get("PSPDX_ASSET") or author.get("asset") or DEFAULT_ASSET
    asset = pick_asset(rel, pattern)
    if asset["size"] > MAX_ASSET:
        raise SystemExit(f"{asset['name']} is {asset['size']} bytes, over the {MAX_ASSET} cap")
    raw = fetch(asset["browser_download_url"])
    if len(raw) != asset["size"]:
        raise SystemExit(f"{asset['name']}: got {len(raw)} bytes, GitHub said {asset['size']}")
    release = release_half(rel, asset, raw)
    print(f"  {asset['name']}: {release['size']} bytes, sha256 {release['sha256'][:12]}..., "
          f"root {release['root']!r}, rev {release['rev']}")

    text = render(author, release)
    changed = text != before
    if changed:
        tmp = path.with_name(FILE + ".new")
        tmp.write_text(text)
        tmp.replace(path)                     # never a half-written file
        print(f"-> {path}")
    else:
        print(f"  {FILE} is already current")
    if o["write_only"]:
        return 0

    if changed:
        commit(cwd, branch, release["version"])
        print(f"  committed to {branch}")
    upload(owner, repo, rel, text)
    print(f"  attached {FILE} to {tag}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except SystemExit as e:
        # An annotation puts the reason on the run's front page; the plain
        # message still goes to the log and the exit code is still 1.
        if os.environ.get("GITHUB_ACTIONS") and isinstance(e.code, str):
            print("::error::" + e.code.strip().replace("\n", "%0A"))
        raise
