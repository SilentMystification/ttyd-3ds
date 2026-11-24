# Build image for the 3DS port. Used by tools/port3ds/build.sh (Linux) and
# tools/port3ds/build.ps1 (Windows) and by CI.
#
# The base image is pinned by digest so every machine builds with the same
# devkitARM 16.1.0 / libctru 2.7.0 / citro3d 1.7.1. The CIA tools are pinned
# downloads checked against their SHA-256.
#
# makerom and ctrtool are built from source: the release binaries need
# glibc 2.38, and the devkitARM image is Debian bookworm (glibc 2.36).

ARG DEVKITARM_IMAGE=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5

FROM ${DEVKITARM_IMAGE} AS ctrtools
ARG PROJECT_CTR_URL=https://github.com/3DSGuy/Project_CTR/archive/refs/tags/makerom-v0.19.0.tar.gz
ARG PROJECT_CTR_SHA256=446bd23919b7e9fa10540a784202d388a0b93ef4d7165f3990481edd2aa2f946
RUN set -eu; \
    mkdir -p /src /out; cd /src; \
    curl -fsSL -o pctr.tar.gz "$PROJECT_CTR_URL"; \
    echo "$PROJECT_CTR_SHA256  pctr.tar.gz" | sha256sum -c -; \
    tar xzf pctr.tar.gz --strip-components=1; \
    make -C makerom deps -j"$(nproc)"; make -C makerom -j"$(nproc)"; \
    make -C ctrtool deps -j"$(nproc)"; make -C ctrtool -j"$(nproc)"; \
    cp makerom/bin/makerom ctrtool/bin/ctrtool /out/

FROM ${DEVKITARM_IMAGE}
ARG BANNERTOOL_URL=https://github.com/diasurgical/bannertool/releases/download/1.2.0/bannertool.zip
ARG BANNERTOOL_SHA256=69768596f836acb3e3aeaa66e47c6ba560dde813c6dfcd33c8afc25fe29b7524
COPY --from=ctrtools /out/ /opt/ctrtools/bin/
RUN set -eu; \
    cd /tmp; \
    curl -fsSL -o bannertool.zip "$BANNERTOOL_URL"; \
    echo "$BANNERTOOL_SHA256  bannertool.zip" | sha256sum -c -; \
    unzip -q -j bannertool.zip linux-x86_64/bannertool -d /opt/ctrtools/bin; \
    rm bannertool.zip; \
    chmod +x /opt/ctrtools/bin/*; \
    /opt/ctrtools/bin/makerom 2>&1 | head -1; \
    /opt/ctrtools/bin/ctrtool 2>&1 | head -1; \
    /opt/ctrtools/bin/bannertool 2>&1 | head -1

# ccache (bookworm packages, pinned by SHA-256; apt is not used because it hangs in this build network):
# build.sh points CMake's compiler launcher at it and mounts the shared cache from the host.
RUN set -eu; \
    cd /tmp; \
    M=http://deb.debian.org/debian/pool/main; \
    curl -fsSL -o zstd.deb "$M/libz/libzstd/libzstd1_1.5.4+dfsg2-5_amd64.deb"; \
    echo "6315b5ac38b724a710fb96bf1042019398cb656718b1522279a5185ed39318fa  zstd.deb" | sha256sum -c -; \
    curl -fsSL -o hiredis.deb "$M/h/hiredis/libhiredis0.14_0.14.1-3_amd64.deb"; \
    echo "61f25d841052ad6cd6adfa9fad050345ea1047abc6ea79f931ab1a9ed55af3ae  hiredis.deb" | sha256sum -c -; \
    curl -fsSL -o ccache.deb "$M/c/ccache/ccache_4.8+really4.7.5-1_amd64.deb"; \
    echo "d6562e03b90d4bbe299966117aef44975f57bea8270c233b1d71f1e6ba157e7d  ccache.deb" | sha256sum -c -; \
    dpkg -i zstd.deb hiredis.deb ccache.deb; \
    rm -f zstd.deb hiredis.deb ccache.deb; \
    ccache --version | head -1

ENV PATH=/opt/ctrtools/bin:$PATH
WORKDIR /work
