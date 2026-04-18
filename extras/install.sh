#!/bin/bash
set -e  # exit on any error

if [ "$EUID" -ne 0 ]; then
    echo "Must run ${0##*/} with sudo or as root"
    exit 1
fi

APP="a4"
VER="VERSION"

# Detect architecture
case "$(uname -m)" in
    x86_64)  ARCH="x86_64" ;;
    aarch64) ARCH="arm64" ;;
    armv7l|armv6l) ARCH="armv7" ;;
    *) echo "Unsupported architecture, $(uname -m), please submit an issue"; exit 1 ;;
esac

# Detect musl libc
if ldd /bin/sh 2>&1 | grep -qi musl; then
    ARCH="$ARCH-musl"
fi

SRC="https://github.com/rpmohn/$APP/releases/download/$VER/$APP-$VER-$ARCH.tar.gz"

DEST="/usr/local"
DEST_BIN="$DEST/bin"
DEST_DATA="$DEST/share"

cleanup() { if [ -e "$1" ] || [ -L "$1" ]; then rm -rf "$1"; fi; }

echo "Installing $APP to $DEST ..."

mkdir -p "$DEST_BIN" "$DEST_DATA/$APP" "$DEST_DATA/man/man1"
curl -fsSL "$SRC" | tar -xzv --strip-components=1 -C "$DEST_DATA/$APP"

cleanup "$DEST_BIN/$APP"
ln -s "$DEST_DATA/$APP/$APP" "$DEST_BIN/$APP"

cleanup "$DEST_DATA/man/man1/$APP.1"
ln -s "$DEST_DATA/$APP/$APP.1" "$DEST_DATA/man/man1/$APP.1"

echo "Successfully installed $APP $VER to $DEST"
