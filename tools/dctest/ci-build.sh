#!/bin/bash
# Builds KallistiOS and dctest.elf. Meant to run inside the KOS toolchain image
# (pcercuei/dreamcast-toolchain:stable), the way the workflow does:
#   tools/dctest/ci-build.sh <KallistiOS checkout> <output dir> <version string>
set -euo pipefail
kos=$(realpath "$1"); out=$(realpath -m "$2"); ver=${3:-dev}
here=$(dirname "$(realpath "$0")")
cd "$kos"
cp doc/environ.sh.sample environ.sh
sed -i "s|^export KOS_BASE=.*|export KOS_BASE=\"$kos\"|" environ.sh
set +u; . ./environ.sh; set -u
make -j"$(nproc)"
cd "$here"
make clean
make DT_BUILD="$ver"
mkdir -p "$out"
cp dctest.elf "$out/dctest-$ver.elf"
ls -l "$out"
