#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${DIST_DIR:-${ROOT_DIR}/dist}"
STAGE_DIR="${STAGE_DIR:-${ROOT_DIR}/build/deb-root}"
BUILD_JOBS="${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
SKIP_BUILD="${SKIP_BUILD:-0}"

PACKAGE_ID="${PACKAGE_ID:-capgps}"
OUTPUT_NAME="${OUTPUT_NAME:-CapGPS}"
PACKAGE_VERSION="${PACKAGE_VERSION:-0.1.0}"
PACKAGE_SUFFIX="${PACKAGE_SUFFIX:-m5stack1}"
DEB_ARCH="${DEB_ARCH:-arm64}"
MAINTAINER="${MAINTAINER:-M5Stack}"

BIN_NAME="M5CardputerZero-CapGPS"
BIN_SRC="${ROOT_DIR}/dist/${BIN_NAME}"
DESKTOP_SRC="${ROOT_DIR}/assets/applications/capgps.desktop"

build_for_cardputerzero() {
    echo "Rebuilding ${BIN_NAME} with CardputerZero=y ..."
    (
        cd "${ROOT_DIR}"
        env CardputerZero=y scons distclean
        env CardputerZero=y scons -j"${BUILD_JOBS}"
    )
}

verify_arm64_binary() {
    local bin_path="$1"
    local file_output
    file_output="$(file "${bin_path}")"
    echo "${file_output}"
    if [[ "${file_output}" != *"aarch64"* && "${file_output}" != *"ARM aarch64"* ]]; then
        echo "Expected an aarch64 binary, but got something else." >&2
        exit 1
    fi
}

if ! command -v dpkg-deb >/dev/null 2>&1; then
    echo "dpkg-deb not found. Install dpkg-dev first." >&2
    exit 1
fi

if ! command -v file >/dev/null 2>&1; then
    echo "file command not found. Install file first." >&2
    exit 1
fi

if [[ ! -f "${DESKTOP_SRC}" ]]; then
    echo "Desktop file not found: ${DESKTOP_SRC}" >&2
    exit 1
fi

if [[ "${SKIP_BUILD}" != "1" ]]; then
    build_for_cardputerzero
elif [[ ! -x "${BIN_SRC}" ]]; then
    echo "Binary not found: ${BIN_SRC}" >&2
    echo "Run: cd ${ROOT_DIR} && env CardputerZero=y scons -j\$(nproc)" >&2
    exit 1
fi

if [[ ! -x "${BIN_SRC}" ]]; then
    echo "Binary not found after build: ${BIN_SRC}" >&2
    exit 1
fi

verify_arm64_binary "${BIN_SRC}"

rm -rf "${STAGE_DIR}"
mkdir -p \
    "${STAGE_DIR}/DEBIAN" \
    "${STAGE_DIR}/usr/share/APPLaunch/bin" \
    "${STAGE_DIR}/usr/share/APPLaunch/applications" \
    "${DIST_DIR}"

install -Dm755 "${BIN_SRC}" "${STAGE_DIR}/usr/share/APPLaunch/bin/${BIN_NAME}"
install -Dm644 "${DESKTOP_SRC}" "${STAGE_DIR}/usr/share/APPLaunch/applications/capgps.desktop"

INSTALLED_SIZE="$(du -sk "${STAGE_DIR}/usr" | awk '{print $1}')"
cat > "${STAGE_DIR}/DEBIAN/control" <<EOF
Package: ${PACKAGE_ID}
Version: ${PACKAGE_VERSION}
Section: utils
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: ${MAINTAINER}
Depends: libc6, libstdc++6, libgcc-s1
Installed-Size: ${INSTALLED_SIZE}
Description: CapGPS demo app for M5CardputerZero APPLaunch
 GPS search, satellite acquisition, and coordinate display demo.
EOF

DEB_PATH="${DIST_DIR}/${OUTPUT_NAME}_${PACKAGE_VERSION}_${PACKAGE_SUFFIX}_${DEB_ARCH}.deb"
dpkg-deb --build --root-owner-group "${STAGE_DIR}" "${DEB_PATH}"

if [[ "${OUTPUT_NAME}" != "${PACKAGE_ID}" ]]; then
    COMPAT_PATH="${DIST_DIR}/${PACKAGE_ID}_${PACKAGE_VERSION}_${PACKAGE_SUFFIX}_${DEB_ARCH}.deb"
    cp -f "${DEB_PATH}" "${COMPAT_PATH}"
    echo "Generated Debian package: ${DEB_PATH}"
    echo "Compatibility copy: ${COMPAT_PATH}"
else
    echo "Generated Debian package: ${DEB_PATH}"
fi
