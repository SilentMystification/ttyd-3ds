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

ENV PATH=/opt/ctrtools/bin:$PATH
WORKDIR /work
