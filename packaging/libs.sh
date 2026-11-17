#!/usr/bin/env bash
#
# FILE            libs.sh
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# The cor-libs corCep is built on: cloned beside a corCep checkout and built, in the order they use each other -
# for the image (docker/Dockerfile) and the .deb (packaging/deb/build.sh). A workstation has its own checkouts.
#
#   packaging/libs.sh <dir> [<ref>]     clone each lib into <dir>/<lib> at <ref> (default: main), build it
#                                       (BUILD=release: the archives staged at each lib's root)
#
# The libs have no releases (they track main): what a build used is what corCep --version prints.
#
set -euo pipefail

dir=${1:?usage: libs.sh <dir> [<ref>]}
ref=${2:-main}
LIBS=(corBase corLog corAlloc corTree corJson corHash corArgs corHttp corRest corJsonld corNgsild)

mkdir -p "$dir"
for lib in "${LIBS[@]}"; do
  if [ ! -d "$dir/$lib/.git" ]; then
    git clone --quiet --depth 1 --branch "$ref" "https://github.com/SEAMWARE-cor/$lib.git" "$dir/$lib"
  fi
  make -C "$dir/$lib" -j"$(nproc)" BUILD=release > "$dir/$lib.build.log" 2>&1 || { tail -30 "$dir/$lib.build.log"; exit 1; }
  printf '%-10s %s\n' "$lib" "$(git -C "$dir/$lib" rev-parse --short=12 HEAD)"
done
