# Build a 3DS CMake project inside the pinned build container (Windows).
#
#   powershell -NoProfile -File tools\port3ds\build.ps1 [-Project probe] [-CMakeArgs '-DFOO=1']
#
# Same result as tools/port3ds/build.sh on Linux. Needs Docker Desktop.
#
# Building straight from a Windows bind mount is slow (file I/O through the
# VM), so the sources are copied with rsync into a Docker volume
# (ttyd3ds-work) and built there. Only the outputs (.3dsx, .cia, .elf, .map)
# are copied back to build\3ds\<project>\. Game data (iso/, orig/) is never
# copied into the volume.

param(
    [string]$Project = '.',
    [string[]]$CMakeArgs = @()
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$src = "port3ds/$Project"
if (-not (Test-Path (Join-Path $repo "$src/CMakeLists.txt"))) {
    Write-Error "build.ps1: no CMakeLists.txt in $src"
    exit 2
}
$name = if ($Project -eq '.') { 'game' } else { Split-Path -Leaf $Project }
$outHost = Join-Path $repo "build\3ds\$name"
New-Item -ItemType Directory -Force -Path $outHost | Out-Null

# Same tag rule as build.sh: first 12 hex digits of the Dockerfile's SHA-256.
# Hash the LF form so Windows and Linux checkouts give the same tag.
$dockerDir = Join-Path $repo 'tools\port3ds\docker'
$dockerText = [IO.File]::ReadAllText((Join-Path $dockerDir 'Dockerfile')) -replace "`r`n", "`n"
$sha = [Security.Cryptography.SHA256]::Create()
$hash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($dockerText)) | ForEach-Object { $_.ToString('x2') })
$tag = "ttyd3ds-build:$($hash.Substring(0, 12))"

docker image inspect $tag *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "build.ps1: building image $tag"
    docker build -t $tag $dockerDir
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

$script = @'
set -eu
src=$1; name=$2; shift 2
mkdir -p /work/src /work/build
rsync -a --delete \
    --exclude=/.git/ --exclude=/build/ --exclude=/iso/ --exclude=/orig/ \
    /host/ /work/src/
cmake -S "/work/src/$src" -B "/work/build/$name" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" "$@" >/dev/null
cmake --build "/work/build/$name"
find "/work/build/$name" -maxdepth 1 \( -name '*.3dsx' -o -name '*.cia' -o -name '*.elf' -o -name '*.map' \) \
    -exec cp {} /out/ \;
'@ -replace "`r`n", "`n"

docker run --rm `
    -v "${repo}:/host:ro" `
    -v 'ttyd3ds-work:/work' `
    -v "${outHost}:/out" `
    $tag sh -c $script sh $src $name @CMakeArgs
if ($LASTEXITCODE -ne 0) { exit 1 }

Write-Host "build.ps1: outputs in build\3ds\$name"
Get-ChildItem $outHost -Include *.3dsx, *.cia -Recurse | ForEach-Object { $_.FullName }
