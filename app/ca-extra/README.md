# Roots not yet in a distribution's store

One PEM per file. `tools/make-ca-bundle.py` refuses any file here that is not
signed by a root already in the host's own CA store, so putting a certificate
here does not add trust — it only carries trust that already exists to a
console that has no store of its own.

| File | Why |
|---|---|
| `isrg-root-yr.pem` | ISRG's 2026 root, cross-signed by ISRG Root X1. GitHub Pages serves a chain ending in it, and it is newer than the ca-certificates package on most machines. |
