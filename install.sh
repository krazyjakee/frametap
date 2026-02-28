#!/bin/sh
# Frametap installer — downloads the latest CLI binary for your platform.
# Usage: curl -fsSL https://raw.githubusercontent.com/krazyjakee/frametap/main/install.sh | sh

set -e

REPO="krazyjakee/frametap"
INSTALL_DIR="${FRAMETAP_INSTALL_DIR:-/usr/local/bin}"

get_platform() {
    os="$(uname -s)"
    case "$os" in
        Linux)  echo "linux" ;;
        Darwin) echo "macos" ;;
        *)      echo "Unsupported OS: $os" >&2; exit 1 ;;
    esac
}

get_latest_version() {
    curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
        | grep '"tag_name"' \
        | head -1 \
        | sed 's/.*"tag_name": *"//;s/".*//'
}

main() {
    platform="$(get_platform)"
    version="$(get_latest_version)"

    if [ -z "$version" ]; then
        echo "Error: could not determine latest version." >&2
        exit 1
    fi

    url="https://github.com/${REPO}/releases/download/${version}/frametap-cli-${platform}.zip"
    echo "Downloading frametap ${version} for ${platform}..."

    tmpdir="$(mktemp -d)"
    trap 'rm -rf "$tmpdir"' EXIT

    curl -fsSL -o "${tmpdir}/frametap.zip" "$url"
    unzip -q "${tmpdir}/frametap.zip" -d "${tmpdir}"

    if [ ! -w "$INSTALL_DIR" ]; then
        echo "Installing to ${INSTALL_DIR} (requires sudo)..."
        sudo install -m 755 "${tmpdir}/frametap" "${INSTALL_DIR}/frametap"
    else
        install -m 755 "${tmpdir}/frametap" "${INSTALL_DIR}/frametap"
    fi

    echo "frametap ${version} installed to ${INSTALL_DIR}/frametap"
    echo "Run 'frametap --help' to get started."
}

main
