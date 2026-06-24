#!/usr/bin/env bash
# Build PS5 target dependencies inside the SDK image.
#
# Based on:
# https://github.com/itsPLK/ps5-payload-manager/blob/main/build_deps.sh
set -euo pipefail

export PATH="/opt/ps5-payload-sdk/bin:${PATH}"

TEMPDIR=$(mktemp -d)
trap 'rm -rf -- "$TEMPDIR"' EXIT

run_configure() {
    if "$@"; then
        return 0
    fi

    if [ -f config.log ]; then
        echo "=== configure failed; config.log head ===" >&2
        sed -n '1,220p' config.log >&2
        echo "=== configure failed; config.log tail ===" >&2
        tail -n 220 config.log >&2
    fi

    return 1
}

cd "$TEMPDIR"

export CC=prospero-clang
export CXX=prospero-clang++
export AR=prospero-ar
export NM=prospero-nm
export RANLIB=prospero-ranlib

echo "=== 1. Building libmicrohttpd 1.0.1 ==="
wget -O libmicrohttpd.tar.gz https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz
tar xf libmicrohttpd.tar.gz
cd libmicrohttpd-1.0.1
run_configure ./configure --host=x86_64-pc-freebsd12 \
            --disable-shared --enable-static \
            --disable-curl --disable-examples \
            --prefix=/opt/ps5-payload-sdk/target
make -j"$(nproc)"
make install

cd "$TEMPDIR"

echo "=== 2. Building mbedTLS 3.6.0 ==="
wget -O mbedtls.tar.gz https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v3.6.0.tar.gz
tar xf mbedtls.tar.gz
cd mbedtls-*
make CC=prospero-clang AR=prospero-ar RANLIB=prospero-ranlib CFLAGS="-Os" lib -j"$(nproc)"
mkdir -p /opt/ps5-payload-sdk/target/include
cp -r include/mbedtls include/psa /opt/ps5-payload-sdk/target/include/
mkdir -p /opt/ps5-payload-sdk/target/lib
cp library/libmbedtls.a library/libmbedx509.a library/libmbedcrypto.a /opt/ps5-payload-sdk/target/lib/

cd "$TEMPDIR"

echo "=== 3. Building libcurl 8.18.0 ==="
wget -O curl.tar.xz https://curl.haxx.se/download/curl-8.18.0.tar.xz
tar xf curl.tar.xz
cd curl-8.18.0
sed -i 's|define USE_XATTR| |g' src/tool_xattr.h
wget -O ca-bundle.crt https://curl.se/ca/cacert.pem
run_configure ./configure --prefix=/opt/ps5-payload-sdk/target \
            --host=x86_64-pc-freebsd \
            --enable-static --disable-shared \
            --with-mbedtls=/opt/ps5-payload-sdk/target \
            --without-openssl \
            --without-libpsl \
            --disable-docs \
            --disable-ftp --disable-ldap --disable-ldaps \
            --disable-rtsp --disable-proxy --disable-dict \
            --disable-telnet --disable-tftp --disable-pop3 \
            --disable-imap --disable-smb --disable-smtp \
            --disable-gopher --disable-mqtt
make -j"$(nproc)"
make install

echo "=== 4. Deploying CA bundle ==="
mkdir -p /opt/ps5-payload-sdk/target/etc
cp ca-bundle.crt /opt/ps5-payload-sdk/target/etc/ca-bundle.crt

echo "All SDK dependencies built and installed."
