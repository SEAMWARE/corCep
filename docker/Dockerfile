#
# FILE            Dockerfile
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corCep's image: built from this checkout and the cor-libs' main (packaging/libs.sh), run as a user of its own.
#
#   docker build -f docker/Dockerfile -t corcep .
#   docker run --rm --network host corcep --broker http://localhost:1026
#
# Options also come from the environment: CORCEP_<OPTION> (CORCEP_BROKER=http://..., CORCEP_PORT=7602).
#
ARG BASE_IMAGE=ubuntu:26.04

FROM ${BASE_IMAGE} AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential git ca-certificates libssl-dev zlib1g-dev libmicrohttpd-dev \
    && rm -rf /var/lib/apt/lists/*

ARG LIBS_REF=main
COPY packaging/libs.sh /tmp/libs.sh
RUN /tmp/libs.sh /build "$LIBS_REF"

COPY . /build/corCep
WORKDIR /build/corCep
# corRest builds with MHD (its default server - the libs build as in CI); corCep uses none of it: ldd shows no microhttpd
RUN make clean && make -j"$(nproc)" BUILD=release && ./corCep --version \
    && ! ldd corCep | grep -q microhttpd


FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libssl3t64 zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --shell /usr/sbin/nologin corcep \
    && mkdir -p /var/lib/corcep && chown corcep /var/lib/corcep

COPY --from=builder /build/corCep/corCep /usr/bin/corCep
COPY LICENSE /usr/share/doc/corcep/LICENSE

ARG VERSION=unknown
ARG GIT_SHA=unknown
LABEL org.opencontainers.image.title="corCep" \
      org.opencontainers.image.description="An NGSI-LD complex event processor - rules as NGSI-LD entities" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${GIT_SHA}" \
      org.opencontainers.image.source="https://github.com/SEAMWARE/corCep" \
      org.opencontainers.image.licenses="Apache-2.0"

USER corcep
WORKDIR /var/lib/corcep
EXPOSE 7602
ENTRYPOINT ["/usr/bin/corCep"]
