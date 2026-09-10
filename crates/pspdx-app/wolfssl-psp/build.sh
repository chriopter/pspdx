#!/bin/sh
# Builds wolfSSL for the PSP into ./prefix. Run inside the pspdev container:
#   docker run --rm -v "$PWD:/src" -w /src pspdev/pspdev:latest sh wolfssl-psp/build.sh
#
# Two departures from the stock pspdev package, both build flags -- the source
# is untouched upstream:
#
#   CUSTOM_RAND_     Makes wc_GenerateSeed() a wrapper around a function the
#   GENERATE_SEED    application provides, so the /dev/urandom branch -- which
#                    the PSP does not have, and which makes wolfCrypt_Init()
#                    die with WC_INIT_E (-228) -- is never compiled in.
#                    The runtime hook wc_SetSeed_Cb() cannot be used for this:
#                    wolfSSL_Init() overwrites it with wc_GenerateSeed again
#                    (src/ssl.c:6276) before anything of ours can take effect.
#
#   CURVE25519 &c.   X25519 costs a fraction of P-256 on a core with no crypto
#                    hardware -- measured here, five key exchanges: 154 ms
#                    against 1032 ms. Ed25519 is for package signatures,
#                    ChaCha20-Poly1305 for bulk without AES acceleration.
set -e

VER=5.7.0
# The pin. A tag is a pointer and can be moved or deleted; only the hash says
# which bytes we actually built against. Same value pspdev pins in its own
# wolfssl recipe, obtained independently of us.
SHA=2de93e8af588ee856fe67a6d7fce23fc1b226b74d710b0e3946bc8061f6aa18f
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/prefix"

cd /tmp
TARBALL="v${VER}-stable.tar.gz"
[ -f "$TARBALL" ] || wget -q "https://github.com/wolfSSL/wolfssl/archive/refs/tags/$TARBALL"

echo "$SHA  $TARBALL" | sha256sum -c - || {
    echo "wolfssl tarball does not match the pinned hash -- refusing to build" >&2
    exit 1
}
rm -rf "wolfssl-${VER}-stable"
tar xf "v${VER}-stable.tar.gz"
cd "wolfssl-${VER}-stable"

mkdir -p build && cd build
CFLAGS="-DNO_WRITEV -DNO_DEV_RANDOM -DCUSTOM_RAND_GENERATE_SEED=psprandom_seed_raw -include $HERE/psprandom_decl.h" \
  cmake -Wno-dev \
    -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" \
    -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DWOLFSSL_CRYPT_TESTS=OFF -DWOLFSSL_EXAMPLES=OFF \
    -DWOLFSSL_CURL=ON \
    -DWOLFSSL_CURVE25519=yes -DWOLFSSL_ED25519=yes \
    -DWOLFSSL_CHACHA=yes -DWOLFSSL_POLY1305=yes \
    -DWARNING_C_FLAGS=-w .. >/dev/null
make -j"$(nproc)"
make install >/dev/null 2>&1

echo "=== flags im ergebnis ==="
for d in WOLFSSL_TLS13 WC_RNG_SEED_CB HAVE_CURVE25519 HAVE_ED25519 HAVE_CHACHA; do
  printf '%-18s ' "$d"
  grep -qE "^#define $d" "$OUT/include/wolfssl/options.h" && echo AN || echo aus
done
