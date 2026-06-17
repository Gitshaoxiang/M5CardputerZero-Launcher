#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${DIST_DIR:-${ROOT_DIR}/dist}"
STAGE_DIR="${STAGE_DIR:-${ROOT_DIR}/build/deb-root}"

PACKAGE_NAME="${PACKAGE_NAME:-sshclient}"
PACKAGE_VERSION="${PACKAGE_VERSION:-0.1.0}"
PACKAGE_SUFFIX="${PACKAGE_SUFFIX:-m5stack1}"
DEB_ARCH="${DEB_ARCH:-arm64}"
MAINTAINER="${MAINTAINER:-M5Stack}"

BIN_NAME="M5CardputerZero-SSHClient"
BIN_SRC="${ROOT_DIR}/dist/${BIN_NAME}"
DESKTOP_SRC="${ROOT_DIR}/assets/applications/sshclient.desktop"

if ! command -v dpkg-deb >/dev/null 2>&1; then
    echo "dpkg-deb not found. Install dpkg-dev first." >&2
    exit 1
fi

if [[ ! -x "${BIN_SRC}" ]]; then
    echo "Binary not found: ${BIN_SRC}" >&2
    echo "Run: cd ${ROOT_DIR} && scons -j\$(nproc)" >&2
    exit 1
fi

rm -rf "${STAGE_DIR}"
mkdir -p \
    "${STAGE_DIR}/DEBIAN" \
    "${STAGE_DIR}/usr/share/APPLaunch/bin" \
    "${STAGE_DIR}/usr/share/APPLaunch/applications" \
    "${DIST_DIR}"

install -Dm755 "${BIN_SRC}" "${STAGE_DIR}/usr/share/APPLaunch/bin/${BIN_NAME}"
install -Dm644 "${DESKTOP_SRC}" "${STAGE_DIR}/usr/share/APPLaunch/applications/sshclient.desktop"

INSTALLED_SIZE="$(du -sk "${STAGE_DIR}/usr" | awk '{print $1}')"
cat > "${STAGE_DIR}/DEBIAN/control" <<EOF
Package: ${PACKAGE_NAME}
Version: ${PACKAGE_VERSION}
Section: utils
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: ${MAINTAINER}
Depends: libc6, libstdc++6, libgcc-s1
Installed-Size: ${INSTALLED_SIZE}
Description: SSH profile manager for M5CardputerZero APPLaunch
 Manage local SSH profiles and connect from CardputerZero.
EOF

DEB_PATH="${DIST_DIR}/${PACKAGE_NAME}_${PACKAGE_VERSION}_${PACKAGE_SUFFIX}_${DEB_ARCH}.deb"
dpkg-deb --build --root-owner-group "${STAGE_DIR}" "${DEB_PATH}"

echo "Generated Debian package: ${DEB_PATH}"
