#!/usr/bin/env bash
# Builds curl with mbedTLS for the console into deps/tls, once.
#
# The portlib curl (7.69.1) hands TLS to the system's ssl service: every
# read crosses into another process, which decrypts there. This one decrypts
# in the app, with the CPU's AES and PMULL instructions (mbedTLS AESCE).
#
# Runs inside devkitpro/devkita64; makefiles/switch.mk calls it when
# deps/tls/lib/libcurl.a is missing.
set -euo pipefail

MBEDTLS_VERSION=3.6.7
MBEDTLS_SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
CURL_VERSION=8.23.0
CURL_SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=$ROOT/deps/tls
WORK=$ROOT/deps/work
JOBS=$(nproc)

source "$DEVKITPRO/switchvars.sh"

fetch() { # url sha256 file
  if [ ! -f "$WORK/$3" ] || ! echo "$2  $WORK/$3" | sha256sum -c --status; then
    curl -fsSL "$1" -o "$WORK/$3.part"
    echo "$2  $WORK/$3.part" | sha256sum -c --quiet
    mv "$WORK/$3.part" "$WORK/$3"
  fi
}

rm -rf "$PREFIX" "$WORK/mbedtls-$MBEDTLS_VERSION" "$WORK/curl-$CURL_VERSION"
mkdir -p "$WORK"
fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2" \
  "$MBEDTLS_SHA256" "mbedtls-$MBEDTLS_VERSION.tar.bz2"
fetch "https://curl.se/download/curl-$CURL_VERSION.tar.xz" "$CURL_SHA256" "curl-$CURL_VERSION.tar.xz"
tar -C "$WORK" -xf "$WORK/mbedtls-$MBEDTLS_VERSION.tar.bz2"
tar -C "$WORK" -xf "$WORK/curl-$CURL_VERSION.tar.xz"

# mbedTLS: entropy and the millisecond clock from the console (both in
# source/platform/TlsPlatform.cpp), AES and SHA-256 on the CPU's crypto
# instructions only (every Switch has them), and locks for the PSA key
# store, which TLS 1.3 shares across the download threads.
cd "$WORK/mbedtls-$MBEDTLS_VERSION"
config() { python3 scripts/config.py "$@"; }
config set MBEDTLS_ENTROPY_HARDWARE_ALT
config set MBEDTLS_NO_PLATFORM_ENTROPY
config set MBEDTLS_PLATFORM_MS_TIME_ALT
config set MBEDTLS_AES_USE_HARDWARE_ONLY
config set MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_ONLY
config set MBEDTLS_THREADING_C
config set MBEDTLS_THREADING_PTHREAD
config unset MBEDTLS_NET_C
config unset MBEDTLS_TIMING_C
config unset MBEDTLS_SELF_TEST
aarch64-none-elf-cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_C_FLAGS="$CFLAGS $CPPFLAGS" \
  -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
cmake --build build -j"$JOBS"
cmake --install build

# curl: HTTP(S) only, mbedTLS, zlib from the portlibs, the RomFS CA list
# for a handle that names none, and its own basename()
# (newlib declares one the console lacks). Our headers go
# first: the portlibs hold mbedTLS 2.28's.
cd "$WORK/curl-$CURL_VERSION"
# fd_set lives in sys/select.h on the console too (devkitPro's patch).
sed -i 's|    defined(__sun__) \|\| defined(__serenity__)|    defined(__SWITCH__) \|\| defined(__sun__) \|\| defined(__serenity__)|' include/curl/curl.h
grep -q 'defined(__SWITCH__)' include/curl/curl.h
aarch64-none-elf-cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_C_FLAGS="-I$PREFIX/include $CFLAGS $CPPFLAGS" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
  -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DBUILD_EXAMPLES=OFF \
  -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_FTP=ON -DCURL_DISABLE_GOPHER=ON \
  -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_IPFS=ON -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON \
  -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_POP3=ON -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_SMB=ON \
  -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_WEBSOCKETS=ON \
  -DCURL_DISABLE_SOCKETPAIR=ON -DCURL_DISABLE_ALTSVC=ON -DCURL_DISABLE_HSTS=ON -DCURL_DISABLE_COOKIES=ON \
  -DCURL_DISABLE_NETRC=ON -DCURL_DISABLE_PROXY=OFF -DCURL_DISABLE_SRP=ON \
  -DENABLE_IPV6=OFF -DENABLE_UNIX_SOCKETS=OFF -DENABLE_THREADED_RESOLVER=OFF \
  -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF \
  -DMBEDTLS_INCLUDE_DIR="$PREFIX/include" \
  -DMBEDTLS_LIBRARY="$PREFIX/lib/libmbedtls.a" \
  -DMBEDX509_LIBRARY="$PREFIX/lib/libmbedx509.a" \
  -DMBEDCRYPTO_LIBRARY="$PREFIX/lib/libmbedcrypto.a" \
  -DCURL_ZLIB=ON -DZLIB_ROOT="$PORTLIBS_PREFIX" \
  -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF \
  -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_USE_GSSAPI=OFF -DCURL_CA_FALLBACK=OFF \
  -DCURL_CA_BUNDLE=romfs:/cacert.pem -DCURL_CA_PATH=none \
  -DHAVE_BASENAME=OFF
cmake --build build -j"$JOBS"
cmake --install build
echo "deps/tls: mbedTLS $MBEDTLS_VERSION, curl $CURL_VERSION"
