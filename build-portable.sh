#!/bin/bash
# Builds the portable (glibc 2.36+) single-file executable via Docker and
# extracts it to dist/domainwizard. See Dockerfile.build and the
# "Packaging" section of the README for why this exists (build.sh alone
# only produces a binary that works on machines with glibc >= the build
# machine's - use this instead of build.sh when you need the result to run
# on a machine other than the one you're building on).
#
# The target machine still needs libgl1/libegl1 (or equivalent) installed -
# see the README for why those aren't bundled.

set -euo pipefail
cd "$(dirname "$0")"

if ! command -v docker >/dev/null; then
    echo "docker not found - install Docker first." >&2
    exit 1
fi

IMAGE=domainwizard-build
CONTAINER=domainwizard-build-extract-$$

docker build -f Dockerfile.build -t "$IMAGE" .

docker create --name "$CONTAINER" "$IMAGE" >/dev/null
trap 'docker rm "$CONTAINER" >/dev/null 2>&1 || true' EXIT

mkdir -p dist
docker cp "$CONTAINER:/src/dist/domainwizard" ./dist/domainwizard

echo
echo "Built: dist/domainwizard ($(du -h dist/domainwizard | cut -f1))"
