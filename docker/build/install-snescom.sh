#!/usr/bin/env bash
#
# Build + install Bisqwit's snescom/sneslink (the 65816 assembler/linker the SNES
# menu is built with). Same recipe as prepare.sh, kept in its own file so the
# Dockerfile stays readable and the quoting is sane.
set -euo pipefail

ver="${1:-1.8.1.1}"

cd /tmp
wget -q "https://bisqwit.iki.fi/src/arch/snescom-${ver}.tar.gz"
tar xzf "snescom-${ver}.tar.gz"
cd "snescom-${ver}"

# this old gcc tuning flag no longer exists in modern g++
sed -i 's/-minline-stringops-dynamically//g' Makefile.sets

# VERSION must expand to a quoted string literal (it is printed by --version).
make CXXFLAGS='-O2 -std=c++1z -fpermissive -w' \
     CPPFLAGS="-DVERSION='\"${ver}\"' -DUSE_PTHREADS=0 -DHASH_MAP=1 -g -pipe"

install -m755 snescom sneslink /usr/local/bin/

cd /
rm -rf "/tmp/snescom-${ver}" "/tmp/snescom-${ver}.tar.gz"
