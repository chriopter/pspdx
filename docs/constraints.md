# What the hardware forces

Most of the design falls out of four facts about the machine.

## No dependencies

Homebrew is statically linked and lives entirely in `ms0:/PSP/GAME/<name>/`.
There are no shared libraries, no dependency graph, no resolver. A package is a
directory. That removes most of what makes a package manager hard, and moves the
value to discovery, trust, and links that still resolve in ten years.

## Networking is the wall

802.11b, 2.4 GHz, WPA2-PSK without PMF. A meaningful number of present-day access
points are WPA3-only, 5 GHz-only, or require PMF, so some users simply cannot get
a PSP online at all. Real throughput is roughly 1–3 Mbit/s.

The TLS stack (`sceHttps`, with Sony's 2007 CA store) does not reach modern
endpoints. Two ways around it:

- terminate TLS elsewhere — a relay on the LAN, or a plain-HTTP mirror
- sign the payload and stop caring about the transport

The second is better: a signed manifest can travel over plain HTTP, and any
mirror becomes untrusted infrastructure that can fail or omit, but not lie.

Handshakes cost more than bytes here. On a 222 MHz MIPS core the TLS handshake
dominates a small request, so the number of distinct *hosts* matters far more
than the number of requests. Keeping one TLS connection open across requests
is the single most valuable optimisation for an update check.

## Memory

24 MB or so for a user application on a PSP-1000. A multi-megabyte JSON catalog
parsed into a DOM is not an option. Either the catalog stays small enough to hold
whole (roughly 150 KB for a thousand entries with id, name, one-line summary,
category and size) or it is fetched per entry.

Icons and long descriptions are fetched lazily and cached on the stick.

## FAT32 and the battery

No journalling, and PSP users pull the battery. Every install is:

```
tmp/<txn>/   download, verify hash, unpack
             ↓ rename (atomic on the same volume)
PSP/GAME/<id>/
```

with a write-ahead journal that the next launch cleans up. Unpacking 665 files
directly into the target directory is both slow on FAT32 and unrecoverable if
power is lost halfway.

## Device differences

- PSP-1000 has 32 MB RAM, later models 64 MB
- the PSP Go uses `ef0:` instead of `ms0:` — the root must be a variable
- kernel-mode homebrew depends on a specific CFW, and Adrenaline on Vita/PSTV
  blocks some of it

This belongs in the manifest as a compatibility block. An update that cannot run
on the user's device should never be offered.
