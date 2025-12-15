#!/bin/bash
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

# Default values
CURVER="${CURVER:-3.0.4}"
PKG_RELEASE="${PKG_RELEASE:-debian13}"
ARCH="${ARCH:-amd64}"
IMAGE_TAG="${IMAGE_TAG:-proxysql:${CURVER}}"

DEB_FILE="binaries/proxysql_${CURVER}-${PKG_RELEASE}_${ARCH}.deb"

echo "==> Building ProxySQL ${CURVER} deb package..."
docker compose -f docker-compose-build.yaml run --rm ${PKG_RELEASE}_build

if [[ ! -f "$DEB_FILE" ]]; then
    echo "ERROR: Expected deb package not found: $DEB_FILE"
    exit 1
fi

echo "==> Deb package built: $DEB_FILE"

echo "==> Building Docker image: $IMAGE_TAG"
docker build \
    --build-arg DEB_FILE="$DEB_FILE" \
    -t "$IMAGE_TAG" \
    .

echo "==> Done! Image: $IMAGE_TAG"
