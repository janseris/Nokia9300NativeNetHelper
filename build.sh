#!/bin/sh
# Builds bin/nethelper.sis (ARMI, for the Nokia 9300).
# Needs the EKA1 toolchain and the Series 80 SDK, set up by the main repo's
#   symbian-build/toolchain/setup_eka1_toolchain.sh <dir> ~/sym
#   symbian-build/toolchain/setup_s80_sdk.sh S80_DP_2_0_SDK.zip ~/sym
set -e
SYM=${SYM:-$HOME/sym}
HERE=$(cd "$(dirname "$0")" && pwd)
export EPOCROOT=$SYM/s80_20/ PATH=$SYM/wrap:$PATH
cd "$HERE/group"
bldmake bldfiles
abld build armi urel 2>&1 | tee /tmp/nethelper_build.log
if grep -q "Error [0-9]" /tmp/nethelper_build.log; then echo "BUILD FAILED"; exit 1; fi
E=$SYM/s80_20/epoc32
mkdir -p "$HERE/bin"
sed "s|EPOC32|$E|g" "$HERE/sis/nethelper.pkg.in" > "$HERE/bin/nethelper.pkg"
cd "$HERE/bin" && makesis nethelper.pkg nethelper.sis && rm nethelper.pkg
ls -l "$HERE/bin/nethelper.sis"
