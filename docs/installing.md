# Installing

## The archive is already the right shape

`extremetuxracer-psp.zip` is rooted at `PSP/`, so it can be dropped onto a
memory stick as-is. That makes it close to an ideal package format: the installer
does not interpret anything, it moves the subtree under `PSP/` and leaves the
rest.

| In the archive | Goes to | Owned by |
|---|---|---|
| `PSP/GAME/ExtremeTuxRacer/EBOOT.PBP` | `<root>:/PSP/GAME/<id>/` | the package |
| `PSP/GAME/ExtremeTuxRacer/data/**` | same, 660 files | the package |
| `PSP/SYSTEM/controls.ini`, `ppsspp.ini` | **nowhere** | the user |
| `LICENSES/`, `INSTALL.txt`, `build.json` | into the package directory | GPL evidence, travels along |

The EBOOT is 4.5 MB of a 51 MB payload; the rest is textures, courses, sounds and
fifteen translations.

## The rule the `PSP/SYSTEM` files force

Those two files sit outside the package's own directory and would overwrite the
user's emulator settings. The archive's own `INSTALL.txt` says as much — "keep
your existing settings if preferred" — but a package manager cannot leave that to
a human reading a text file.

So: **a package writes only inside its own `GAME/<id>/`.** Anything outside needs
an explicit entry in the manifest and is never silently overwritten. The same
applies to plugins, which need lines in `seplugins/*.txt`: the manager owns those
lines, keyed by path, and removes them on uninstall.

`PSP/SAVEDATA/` is never touched, on install or uninstall.

## Uninstall needs a manifest

The client records which files it placed, per package, in `<root>:/PSP/PSPDX/db`.
Without that list there is no way to tell which of 665 files came from the
package and which the user added. Anything the application writes at runtime —
saves, settings, screenshots — is not part of the package and stays.

## Naming

The archive says `ExtremeTuxRacer`; the package id is
`io.github.chriopter.extremetuxracer`. One of them has to win, or uninstall does not
know what it is allowed to delete. Using the id as the directory name is the safe
choice, at the cost of a less pretty entry in the XMB — which reads its label
from `PARAM.SFO` anyway, not from the directory.

## Nothing lives in git

Worth stating because it drives the whole mirroring question: the EBOOT is not in
the source repository. psp-tuxracer's `.gitignore` is an allowlist, and only the
source port, build scripts, docs and workflow are tracked. The binary exists
solely as a release asset — which can be deleted, or replaced under the same
name.
