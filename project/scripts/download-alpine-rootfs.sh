#!/usr/bin/env bash
set -euo pipefail

DEST="${1:-/tmp/minidocker/rootfs}"
VERSION="${ALPINE_VERSION:-3.19}"
ARCH="${ARCH:-x86_64}"
URL="https://dl-cdn.alpinelinux.org/alpine/v${VERSION}/releases/${ARCH}/alpine-minirootfs-${VERSION}.0-${ARCH}.tar.gz"

mkdir -p "$DEST"
tmp=$(mktemp)
echo "Downloading $URL"
curl -fLo "$tmp" "$URL"
echo "Extracting into $DEST"
tar -xzf "$tmp" -C "$DEST"
rm -f "$tmp"
echo "Alpine rootfs ready at $DEST"