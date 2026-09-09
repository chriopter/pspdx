# pspdx-app

The on-device client, built with [rust-psp](https://github.com/overdrivenpotato/rust-psp).
Right now it draws four lines and waits for HOME.

## Building

Needs `rustup`, the nightly rust-psp pins, and `cargo-psp`:

```sh
rustup toolchain install nightly-2026-08-26 --component rust-src
cargo install --git https://github.com/overdrivenpotato/rust-psp cargo-psp
RUSTUP_TOOLCHAIN=nightly-2026-08-26 cargo psp
```

The result is `target/mipsel-sony-psp/debug/pspdx-app.EBOOT.PBP`. Copy it to
`ms0:/PSP/GAME/pspdx/EBOOT.PBP`, or open it in PPSSPP.

CI builds the same thing on every push and attaches it as an artifact.
