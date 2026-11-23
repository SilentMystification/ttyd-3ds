#!/bin/sh
# Build a 3DS CMake project inside the pinned build container.
#
#   tools/port3ds/build.sh [project] [cmake -D options...]
#   tools/port3ds/build.sh . -DREGION=PAL     (the game for one region; US by default)
#
# project is a directory under port3ds/ ("probe" builds port3ds/probe; the
# default "." builds the game in port3ds/), or a path from the repository
# root ("libs/ctr-devkit" builds the ctr-devkit examples). Output goes to
# build/3ds/<last path component>/.
# The image is built from tools/port3ds/docker/Dockerfile on first use and
# tagged with a hash of the Dockerfile, so a changed Dockerfile rebuilds it.
#
# Windows uses tools/port3ds/build.ps1, which runs the same container.

set -eu

repo=$(cd "$(dirname "$0")/../.." && pwd)
project=${1:-.}
[ $# -gt 0 ] && shift

if [ "$project" != "." ] && [ -f "$repo/$project/CMakeLists.txt" ]; then
    src=$project
else
    src="port3ds/$project"
fi
name=$(basename "$(cd "$repo/$src" && pwd)")
if [ "$project" = "." ]; then
    # The game builds once per region (-DREGION=US|JP|PAL or -DTTYD_VERSION=<id>):
    # US in build/3ds/game, JP in build/3ds/game_jp, PAL in build/3ds/game_pal.
    name=game
    for a in "$@"; do
        case "$a" in
            -DREGION=JP|-DREGION=jp|-DTTYD_VERSION=G8MJ01) name=game_jp ;;
            -DREGION=PAL|-DREGION=pal|-DTTYD_VERSION=G8MP01) name=game_pal ;;
            -DREGION=US|-DREGION=us|-DTTYD_VERSION=G8ME01) name=game ;;
        esac
    done
fi
out="build/3ds/$name"

[ -f "$repo/$src/CMakeLists.txt" ] || { echo "build.sh: no CMakeLists.txt in $src" >&2; exit 2; }

dockerfile="$repo/tools/port3ds/docker/Dockerfile"
tag="ttyd3ds-build:$(sha256sum "$dockerfile" | cut -c1-12)"
if ! docker image inspect "$tag" >/dev/null 2>&1; then
    echo "build.sh: building image $tag"
    docker build -t "$tag" "$repo/tools/port3ds/docker"
fi

# In a git worktree, .git is a file pointing into the main repository's .git:
# mount that too (read-only), so the build string gets the commit.
gitmount=
if [ -f "$repo/.git" ]; then
    common=$(cd "$repo" && cd "$(git rev-parse --git-common-dir)" && pwd)
    gitmount="-v $common:$common:ro"
fi

# The game embeds the SJIS ROM font (CMakeLists.txt). It is rendered on the host, where Pillow is, into
# build/fonts/ (untracked); the font is downloaded once (pinned, SHA-256 checked).
if [ "$project" = "." ]; then
    sjis="$repo/build/fonts/sjis_font.bin"
    if [ ! -f "$sjis" ] || [ "$repo/tools/port3ds/gen_sjis_font.py" -nt "$sjis" ]; then
        python3 -I "$repo/tools/port3ds/gen_sjis_font.py" ||
            echo "build.sh: warning: no SJIS ROM font (needs Python Pillow); the JP build will not configure" >&2
    fi
fi

# Machine-wide limit on concurrent builds (BUILD_MAX, default 2): one flock'd slot file per build, held on fd 9
# until this script ends (the OS drops the lock if it dies). Each build runs ninja with BUILD_JOBS cores.
build_max=${BUILD_MAX:-2}
build_jobs=${BUILD_JOBS:-8}
lockdir="${XDG_CACHE_HOME:-$HOME/.cache}/ttyd3ds"
mkdir -p "$lockdir"
told=
while :; do
    i=0
    got=
    while [ "$i" -lt "$build_max" ]; do
        exec 9>"$lockdir/build-slot-$i"
        if flock -n 9; then
            got=1
            break
        fi
        exec 9>&-
        i=$((i + 1))
    done
    [ -n "$got" ] && break
    if [ -z "$told" ]; then
        echo "build.sh: waiting for a free build slot ($build_max in use)"
        told=1
    fi
    sleep 5
done

mkdir -p "$repo/$out"
# shellcheck disable=SC2086
docker run --rm $gitmount \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e "BUILD_JOBS=$build_jobs" \
    -v "$repo:/work" \
    -w /work \
    "$tag" \
    sh -c 's=$1; o=$2; shift 2; cmake -S "$s" -B "$o" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake "$@" >/dev/null && cmake --build "$o" -j "$BUILD_JOBS"' \
    sh "$src" "$out" "$@"

# Archive the game's ELF by build string, so tools/port3ds/crash_report.py can
# resolve the addresses of a crash report from any earlier build.
if [ "${name%%_*}" = game ] && [ -f "$repo/$out/ttyd.elf" ] && [ -f "$repo/$out/generated/port3ds_build.h" ]; then
    id=$(sed -n 's/.*PORT3DS_BUILD_STRING "#\([0-9]*\) \([0-9a-f]*\).*/\1_\2/p' "$repo/$out/generated/port3ds_build.h")
    # JP and PAL builds of the same commit get a region suffix (<build>_jp.elf).
    [ "$name" != game ] && id="${id}_${name#game_}"
    if [ -n "$id" ]; then
        mkdir -p "$repo/build/elfs"
        cp "$repo/$out/ttyd.elf" "$repo/build/elfs/$id.elf"
        echo "build.sh: ELF archived as build/elfs/$id.elf"
        # The incoming-build text for devupdate.py push (a sidecar next to the CIA).
        sed -n 's/.*PORT3DS_BUILD_STRING "\(#[0-9]* [0-9a-f]*\).*/\1/p' "$repo/$out/generated/port3ds_build.h" > "$repo/$out/ttyd.version"
    fi
fi

echo "build.sh: outputs in $out"
ls "$repo/$out"/*.3dsx "$repo/$out"/*.cia 2>/dev/null || true
