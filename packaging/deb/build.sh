#!/usr/bin/env bash
#
# FILE            build.sh
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corcep_<version>+<dist>[.<revision>]_<arch>.deb - built in a container of the distribution it is for (Ubuntu 26.04,
# 24.04, Debian 13: .github/workflows/packages.yml), from this checkout and the cor-libs' main (packaging/libs.sh).
#
#   packaging/deb/build.sh <out dir>        DEB_REVISION (a package revision of one version: .<N>), DEB_MAINTAINER;
#                                           run as root in the container
#
# The version as coraine's packages have it (the same apt repository: coraine's packaging/deb/publish.sh files a
# package under its distribution by the +<dist> suffix): 0.1.0+ubuntu26.04, a package revision 0.1.0+debian13.1.
#
# The package: /usr/bin/corCep, the systemd unit corcep.service (as the user corcep, its state in
# /var/lib/corcep), /etc/default/corcep (the options - a conffile). Installed: enabled and started; the broker by
# default a coraine on the same host.
#
set -euo pipefail

out=$(realpath -m "${1:?usage: build.sh <out dir>}")
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
rev=${DEB_REVISION:-}
maintainer=${DEB_MAINTAINER:-Ken Zangelin <ken.zangelin@seamware.com>}   # as coraine's packages

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential git ca-certificates libssl-dev zlib1g-dev libmicrohttpd-dev dpkg-dev lintian > /dev/null

. /etc/os-release
dist="${ID}${VERSION_ID}"
arch=$(dpkg --print-architecture)
version=$(sed -n 's/^#define CORCEP_VERSION *"\(.*\)"/\1/p' "$src/src/corCep.h")
full="${version}+${dist}${rev:+.$rev}"

#
# Build - the libs beside a copy of corCep, as the makefile wants them (siblings)
#
work=$(mktemp -d)
"$src/packaging/libs.sh" "$work"
git config --global --add safe.directory '*'
cp -a "$src" "$work/corCep"
make -C "$work/corCep" clean > /dev/null
make -C "$work/corCep" -j"$(nproc)" BUILD=release > "$work/corCep.build.log" 2>&1 || { tail -30 "$work/corCep.build.log"; exit 1; }
"$work/corCep/corCep" --version
if ldd "$work/corCep/corCep" | grep -q microhttpd; then echo "corCep links libmicrohttpd - it must not" >&2; exit 1; fi

#
# The tree
#
root="$work/root"
install -D -m 755 "$work/corCep/corCep"           "$root/usr/bin/corCep"
install -D -m 644 "$here/corcep.service"          "$root/usr/lib/systemd/system/corcep.service"
install -D -m 644 "$here/corcep.default"          "$root/etc/default/corcep"
install -d "$root/usr/share/doc/corcep"
cat > "$root/usr/share/doc/corcep/copyright" <<COPYRIGHT
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: corCep
Source: https://github.com/SEAMWARE/corCep

Files: *
Copyright: 2026 Seamware
License: Apache-2.0
 On Debian systems, the full text of the Apache License 2.0 is in
 /usr/share/common-licenses/Apache-2.0.
COPYRIGHT
printf 'corcep (%s) unstable; urgency=medium\n\n  * corCep %s - the changes: https://github.com/SEAMWARE/corCep/commits/main\n\n -- %s  %s\n' \
       "$full" "$version" "$maintainer" "$(date -R)" | gzip -9n > "$root/usr/share/doc/corcep/changelog.gz"   # native: no Debian revision
install -D -m 644 "$src/README.md"                "$root/usr/share/doc/corcep/README.md"
strip --strip-unneeded "$root/usr/bin/corCep"

install -d "$root/DEBIAN"
for s in postinst prerm postrm; do install -m 755 "$here/$s" "$root/DEBIAN/$s"; done
echo "/etc/default/corcep" > "$root/DEBIAN/conffiles"

#
# Depends - what the binary needs, as the distribution names it (dpkg-shlibdeps)
#
mkdir -p "$work/shlib/debian"
touch "$work/shlib/debian/control"
depends=$(cd "$work/shlib" && dpkg-shlibdeps -O "$root/usr/bin/corCep" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')

cat > "$root/DEBIAN/control" <<CONTROL
Package: corcep
Version: $full
Architecture: $arch
Maintainer: $maintainer
Section: net
Priority: optional
Homepage: https://github.com/SEAMWARE/corCep
Depends: $depends, adduser
Installed-Size: $(du -sk "$root" | cut -f1)
Description: NGSI-LD complex event processor
 corCep runs rules kept as NGSI-LD entities in the broker: patterns over the
 notified changes (sequences, absences, windows, schedules) and actions back
 on the broker (set, toggle, step, create, delete).
CONTROL

mkdir -p "$out"
deb="$out/corcep_${full}_${arch}.deb"
dpkg-deb --root-owner-group --build "$root" "$deb" > /dev/null
lintian --no-tag-display-limit "$deb" > "$out/lintian-${dist}-${arch}.txt" 2>&1 || true
echo "built $deb"
