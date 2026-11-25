#!/bin/sh
# Build a 3DS CMake project inside the pinned build container.
#
#   tools/port3ds/build.sh [--dev] [project] [cmake -D options...]
#   tools/port3ds/build.sh . -DREGION=PAL     (the game for one region; US by default)
#   tools/port3ds/build.sh --dev              (fast dev build: LTO and GX -O3 off)
#   tools/port3ds/build.sh --no-cia           (3dsx only: skips makerom, 70-100 s; TTYD_NO_CIA=1 does the same)
#
# project is a directory under port3ds/ ("probe" builds port3ds/probe; the
# default "." builds the game in port3ds/), or a path from the repository
# root ("libs/ctr-devkit" builds the ctr-devkit examples). Output goes to
# build/3ds/<last path component>/.
#
# --dev (or TTYD_DEV=1) configures a separate build directory (build/3ds/<name>-dev)
# with -DTTYD_LTO=OFF -DTTYD_GX_O3=OFF, so the release directory keeps its LTO build.
# Explicit -D options after it still win. Dev builds do not archive the ELF.
#
# TTYD_CCACHE=1 sends compile jobs through ccache (cache shared in ${XDG_CACHE_HOME:-~/.cache}/ttyd3ds/ccache).
# The ELF of a ccache build is byte-identical to a no-ccache build of the same commit (checked: cold, warm).
# (CCACHE_BASEDIR used to break that: it made the debug paths and LTO names relative.) Still opt-in.
# BUILD_JOBS unset: all host threads when no other build holds a build slot, else 8.
#
# The image is built from tools/port3ds/docker/Dockerfile on first use and
# tagged with a hash of the Dockerfile, so a changed Dockerfile rebuilds it.
#
# Each phase prints its wall time to stderr ("build.sh: time ..."); the compile,
# link and packaging split comes from ninja's log (build/3ds/<name>/.ninja_log).
#
# Windows uses tools/port3ds/build.ps1, which runs the same container.

set -eu

now_ms() { date +%s%3N; }
secs() { [ "$1" -lt 0 ] && set -- 0; printf '%d.%03ds' $(($1 / 1000)) $(($1 % 1000)); }

t_start=$(now_ms)
repo=$(cd "$(dirname "$0")/../.." && pwd)

dev=0
if [ "${TTYD_DEV:-0}" = 1 ]; then dev=1; fi
nocia=0
if [ "${TTYD_NO_CIA:-0}" = 1 ]; then nocia=1; fi
while [ "${1:-}" = --dev ] || [ "${1:-}" = --no-cia ]; do
    if [ "$1" = --dev ]; then dev=1; else nocia=1; fi
    shift
done
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
if [ "$dev" = 1 ]; then
    out="$out-dev"
    # Dev defaults come first, so explicit -D options after them override.
    set -- -DTTYD_LTO=OFF -DTTYD_GX_O3=OFF "$@"
fi

# --no-cia (or TTYD_NO_CIA=1): build the 3dsx only. makerom compresses the code and takes 70-100 s, about
# a quarter of a full build; azahar runs and 3dsx tests do not need the CIA. The option is a cached CMake
# variable, so the default passes ON explicitly. A CIA from an earlier build stays in the directory, stale.
if [ "$nocia" = 1 ]; then set -- -DTTYD_CIA=OFF "$@"; else set -- -DTTYD_CIA=ON "$@"; fi

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
# BUILD_JOBS unset: all host threads when no other build holds a slot, else 8.
build_max=${BUILD_MAX:-2}
build_jobs=${BUILD_JOBS:-}
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
t_lock=$(now_ms)

if [ -z "$build_jobs" ]; then
    busy=
    j=0
    while [ "$j" -lt "$build_max" ]; do
        if [ "$j" != "$i" ]; then
            exec 8>"$lockdir/build-slot-$j"
            flock -n 8 || busy=1
            exec 8>&-
        fi
        j=$((j + 1))
    done
    # Alone: all host threads but two (desktop and emulators stay responsive). Beside another build: 6.
    if [ -z "$busy" ]; then build_jobs=$(( $(nproc) - 2 )); else build_jobs=6; fi
    [ "$build_jobs" -ge 1 ] || build_jobs=1
fi

# ccache (TTYD_CCACHE=0 turns it off): compile jobs go through ccache, whose cache is shared by all
# checkouts in ${XDG_CACHE_HOME:-~/.cache}/ttyd3ds/ccache. A fresh worktree or a checkout of another
# commit recompiles only the sources whose preprocessed text is new to the cache. Only the compile
# lines are wrapped; LTO and link are not.
ccmount=
if [ "${TTYD_CCACHE:-0}" = 1 ]; then
    # One cache for every checkout: sources are always at /work inside the container, so worktrees share hits.
    ccdir="${XDG_CACHE_HOME:-$HOME/.cache}/ttyd3ds/ccache"
    mkdir -p "$ccdir"
    # Sources are always /work inside the container, so absolute paths match across checkouts and
    # NOHASHDIR keeps the (constant) working directory out of the hash. No CCACHE_BASEDIR: it rewrote the
    # absolute source paths on the compile line to relative ones (../../../src/x.c), so the debug info and
    # the LTO section names/symbol suffixes (derived from the paths) differed from a no-ccache build.
    ccmount="-v $ccdir:/ccache -e CCACHE_DIR=/ccache -e CCACHE_MAXSIZE=10G -e CCACHE_NOHASHDIR=true"
    set -- -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache "$@"
else
    # The launcher is a cached CMake variable: clear it, or an earlier ccache build keeps using it.
    set -- -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_ASM_COMPILER_LAUNCHER= "$@"
fi

mkdir -p "$repo/$out"
ninja_log="$repo/$out/.ninja_log"
t_docker=$(now_ms)
# shellcheck disable=SC2086
docker run --rm $gitmount $ccmount \
    --cpu-shares 256 \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e "BUILD_JOBS=$build_jobs" \
    -e "TTYD_T_HOST=$t_docker" \
    -v "$repo:/work" \
    -w /work \
    "$tag" \
    sh -c 's=$1; o=$2; shift 2; t0=$(date +%s%3N); echo "build.sh: container start $((t0 - TTYD_T_HOST)) ms after docker run" >&2; cmake -S "$s" -B "$o" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake "$@" >/dev/null || exit $?; t1=$(date +%s%3N); echo "build.sh: cmake configure $((t1 - t0)) ms" >&2; nice -n 10 cmake --build "$o" -j "$BUILD_JOBS"; rc=$?; t2=$(date +%s%3N); echo "build.sh: cmake build $((t2 - t1)) ms" >&2; exit $rc' \
    sh "$src" "$out" "$@"
t_done=$(now_ms)

# Phase split from ninja's log: only entries whose output was written by this run (mtime, in seconds).
# Ninja times (start, end) are ms from its own start, so only differences are used.
if [ -f "$ninja_log" ]; then
    awk -v t="$((t_docker / 1000))" '
        NF >= 4 && $3 / 1000000000 >= t {
            s = $1 + 0; e = $2 + 0; o = $4
            if (substr(o, 1, 1) == "/") next    # ninja lists each output twice (relative and /work/ path)
            if (o == "ttyd.elf") link = e - s
            else if (o ~ /\.(cia|3dsx)$/) pkg += e - s
            else if (o ~ /^rel\//) { if (rs == "" || s < rs) rs = s; if (e > re) re = e }
            else if (o ~ /\.o$/) { if (cs == "" || s < cs) cs = s; if (e > ce) ce = e }
        }
        END {
            printf "build.sh: ninja split (ms): compile span %d, rel_link span %d, link+post-build %d, makerom/3dsxtool %d\n",
                ce - cs, re - rs, link, pkg
        }' "$ninja_log" >&2
fi
echo "build.sh: time: lock wait $(secs $((t_lock - t_start))), docker start+cmake+build $(secs $((t_done - t_docker))), total $(secs $((t_done - t_start)))" >&2
echo "build.sh: jobs $build_jobs" >&2

# Archive the game's ELF by build string, so tools/port3ds/crash_report.py can
# resolve the addresses of a crash report from any earlier build.
if [ "$dev" = 0 ] && [ "${name%%_*}" = game ] && [ -f "$repo/$out/ttyd.elf" ] && [ -f "$repo/$out/generated/port3ds_build.h" ]; then
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
