#!/bin/bash
# Build a .deb package from a pre-built frametap CLI binary.
# Usage: ./build-deb.sh <path-to-frametap-binary> <version>
# Example: ./build-deb.sh ./frametap 1.0.0

set -euo pipefail

BINARY="$1"
VERSION="$2"
ARCH="amd64"
PKG="frametap_${VERSION}_${ARCH}"

if [ ! -f "$BINARY" ]; then
    echo "Error: binary not found: $BINARY" >&2
    exit 1
fi

rm -rf "$PKG"
mkdir -p "$PKG/DEBIAN"
mkdir -p "$PKG/usr/bin"
mkdir -p "$PKG/usr/share/doc/frametap"

cp "$BINARY" "$PKG/usr/bin/frametap"
chmod 755 "$PKG/usr/bin/frametap"

cat > "$PKG/DEBIAN/control" <<CTRL
Package: frametap
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Depends: libx11-6, libxext6, libxfixes3, libxinerama1, libpipewire-0.3-0, libsystemd0, libwayland-client0
Maintainer: Jacob Cattrall <https://github.com/krazyjakee>
Homepage: https://github.com/krazyjakee/frametap
Description: Cross-platform screen capture CLI
 Frametap lets you take screenshots of your entire screen, a specific
 monitor, a window, or a custom region from the command line.
CTRL

cp "$(dirname "$0")/../../LICENSE" "$PKG/usr/share/doc/frametap/copyright"

dpkg-deb --build "$PKG"
echo "Built $PKG.deb"
