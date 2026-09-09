# Design notes

Ideas, not decisions. Nothing described here is built, and several documents
record approaches that were tried on paper and dropped — the reasoning is kept
because the dead ends are the useful part.

| Document | What it covers |
|---|---|
| [constraints.md](constraints.md) | What the PSP can actually do. Everything else follows from this. |
| [architecture.md](architecture.md) | Two layers: per-package manifests for updates, a catalog for discovery. |
| [manifest.md](manifest.md) | Sketch of the `.pspdx` file and the catalog entry. |
| [updates.md](updates.md) | How a new version becomes visible, and how fast. |
| [installing.md](installing.md) | What lands on the Memory Stick, and what must never be touched. |
| [open-questions.md](open-questions.md) | Unresolved: signing, mirroring, GPL, multiple registries. |

The worked example throughout is
[psp-tuxracer](https://github.com/chriopter/psp-tuxracer), because it is real:
GPL-2.0, GitHub Releases, a 42 MB zip with a 240 MB source tarball beside it,
and sixteen releases in a single day.
