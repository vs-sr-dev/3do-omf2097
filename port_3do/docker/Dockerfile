## 3DO build environment for OpenOMF port
##
## Self-contained image: clones Trapexit's 3do-devkit at build time, so the
## image works on any host without needing the user's WSL devkit copy.
##
## Build (from project root):
##   docker build -t omf2097-3do -f port_3do/docker/Dockerfile port_3do/docker
##
## Run (from project root):
##   docker run --rm -v "${PWD}:/work" -w /work/port_3do omf2097-3do make
##
## Phase 0 NOTE: This Dockerfile is provided as the portable build option, but
## the primary Phase 0 driver is build.ps1 which calls WSL directly — faster
## iteration and avoids a multi-GB image rebuild while we're still validating
## that the port is even feasible.

FROM debian:bookworm-slim

ARG DEBIAN_FRONTEND=noninteractive
ARG DEVKIT_REF=master

RUN apt-get update && apt-get install -y --no-install-recommends \
        git \
        make \
        bash \
        coreutils \
        findutils \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch ${DEVKIT_REF} \
        https://github.com/trapexit/3do-devkit.git /opt/3do-devkit

ENV TDO_DEVKIT_PATH=/opt/3do-devkit
ENV PATH="/opt/3do-devkit/bin/compiler/linux:/opt/3do-devkit/bin/tools/linux:/opt/3do-devkit/bin/buildtools/linux:${PATH}"

WORKDIR /work

CMD ["make"]
