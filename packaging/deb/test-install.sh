#!/usr/bin/env bash
#
# FILE            test-install.sh
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# A built package in a CLEAN container of its distribution: apt resolves its dependencies, the files are where
# they should be, corCep runs, the user exists; then a purge leaves nothing behind.
#
#   packaging/deb/test-install.sh <dir with the .deb>
#
set -euo pipefail

deb=$(ls "${1:?usage: test-install.sh <dir>}"/corcep_*.deb | head -1)
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq "$(realpath "$deb")" > /dev/null

corCep --version
test -f /usr/lib/systemd/system/corcep.service
grep -q '^CORCEP_BROKER=' /etc/default/corcep
getent passwd corcep > /dev/null
test "$(stat -c %U /var/lib/corcep)" = corcep
usage=$(corCep 2>&1 || true)                                             # exits 1: no --broker
grep -q -- "missing required option: --broker" <<< "$usage" || { echo "corCep without --broker: $usage" >&2; exit 1; }
su -s /bin/sh corcep -c "corCep --version" > /dev/null                  # runs as its user

apt-get purge -y -qq corcep > /dev/null
! command -v corCep
! getent passwd corcep > /dev/null
! test -e /var/lib/corcep
echo "test-install: $(basename "$deb") - ok"
