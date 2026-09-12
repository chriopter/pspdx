# The release action

Keeps the release half of `app.pspdx` true. The format is
[manifest.md](../manifest.md); this is the part of it that runs.

## In the author's repository

Two ways in, depending on who makes the release.

**Releases made by hand**, on the GitHub page or with `gh` from a desk:
`.github/workflows/pspdx.yml`, three lines and a job.

```yaml
on:
  release:
    types: [published]
permissions:
  contents: write
jobs:
  pspdx:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: chriopter/pspdx/action@master
```

**Releases made by the repository's own workflow**, with `gh release create`
on a `v*` tag: a release created with `GITHUB_TOKEN` fires no `release`
event, so the workflow above would never run. Put the action at the end of
the workflow that makes the release instead, after the zip is on it, and
tell it the tag.

```yaml
      - run: gh release create "$GITHUB_REF_NAME" the-app.zip
        env:
          GH_TOKEN: ${{ github.token }}
      - uses: chriopter/pspdx/action@master
        with:
          tag: ${{ github.ref_name }}
```

That workflow needs `permissions: contents: write` as well, and an
`actions/checkout@v4` step somewhere before.

Either way the action

1. picks the zip: the only one on the release, or the one the `asset` glob
   in `app.pspdx` names (`"asset": "*-psp.zip"`), or the one the `asset`
   input to the action names;
2. downloads it, takes its sha256 and size from the bytes it got, and finds
   the directory of the shallowest `EBOOT.PBP` inside, which becomes `root`;
3. checks out the default branch (the workflow is on the tag, detached) and
   reads the author's half of `app.pspdx` there (`id`, `name`, `author`,
   `summary`, `category`, `license`, `repo`, and `asset` if there is one),
   holding it to the rules in the manifest. A repository without the file
   gets a first draft from its own name, owner, description and licence, and
   the log says so; the draft is a guess to be corrected by hand, the same
   as [app.pspdx.template](../app.pspdx.template) is;
4. writes `version` (the tag without its `v`), `rev` (the release's
   `published_at` as unix seconds), `url`, `sha256`, `size` and `root` under
   the author's half;
5. commits the file to the default branch as `pspdx-release`, with the
   message `app.pspdx for <version>`, unless nothing in it changed;
6. attaches a copy to the release, replacing one from an earlier run. A
   list that pins a release with `@tag` reads that copy.

A prerelease is left alone: the console installs the highest `rev` it sees.
A file that breaks a rule fails the job and the reason is on the run's front
page. The author's half is never rewritten by the action: it is read, checked
and put back as it was.

Inputs: `token` (default `${{ github.token }}`), `asset` (a glob, default
none) and `tag` (default none, meaning the `release` event's payload).

## On a desk

The same script writes the file for a repository whose workflow is not wired
up yet, or to see what the action would do:

```
cd path/to/the-app-repo
python3 path/to/pspdx/action/release.py --repo owner/name --tag v1.2.3 --write-only
```

`--tag` may be left out for the newest release, and `--asset` names the zip
when there is more than one. The token comes from `GITHUB_TOKEN` or from
`gh auth token`; without one it still works, 60 calls an hour. The file lands
in the current directory and nothing is committed or uploaded; that part is
by hand.
