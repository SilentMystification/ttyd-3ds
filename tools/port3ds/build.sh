#!/bin/sh
# Build a 3DS CMake project inside the pinned build container.
#
#   tools/port3ds/build.sh [project] [cmake -D options...]
#
# project is a directory under port3ds/ ("probe" builds port3ds/probe; the
# default "." builds the game in port3ds/). Output goes to build/3ds/<project>/.
# The image is built from tools/port3ds/docker/Dockerfile on first use and
# tagged with a hash of the Dockerfile, so a changed Dockerfile rebuilds it.
#
# Windows uses tools/port3ds/build.ps1, which runs the same container.

set -eu

repo=$(cd "$(dirname "$0")/../.." && pwd)
project=${1:-.}
[ $# -gt 0 ] && shift

src="port3ds/$project"
name=$(basename "$(cd "$repo/$src" && pwd)")
[ "$project" = "." ] && name=game
out="build/3ds/$name"

[ -f "$repo/$src/CMakeLists.txt" ] || { echo "build.sh: no CMakeLists.txt in $src" >&2; exit 2; }

dockerfile="$repo/tools/port3ds/docker/Dockerfile"
tag="ttyd3ds-build:$(sha256sum "$dockerfile" | cut -c1-12)"
if ! docker image inspect "$tag" >/dev/null 2>&1; then
    echo "build.sh: building image $tag"
    docker build -t "$tag" "$repo/tools/port3ds/docker"
fi

mkdir -p "$repo/$out"
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -v "$repo:/work" \
    -w /work \
    "$tag" \
    sh -c 's=$1; o=$2; shift 2; cmake -S "$s" -B "$o" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake "$@" >/dev/null && cmake --build "$o"' \
    sh "$src" "$out" "$@"

echo "build.sh: outputs in $out"
ls "$repo/$out"/*.3dsx "$repo/$out"/*.cia 2>/dev/null || true
