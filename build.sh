#!/bin/sh
# Builds bin/pomocnik.sis (ARMI, for the Nokia 9300).
# Needs the EKA1 toolchain and the Series 80 SDK, set up by the main repo's
#   symbian-build/toolchain/setup_eka1_toolchain.sh <dir> ~/sym
#   symbian-build/toolchain/setup_s80_sdk.sh S80_DP_2_0_SDK.zip ~/sym
set -e
SYM=${SYM:-$HOME/sym}
HERE=$(cd "$(dirname "$0")" && pwd)
export EPOCROOT=$SYM/s80_20/ PATH=$SYM/wrap:$PATH
cd "$HERE/group"
bldmake bldfiles
abld build armi urel
E=$SYM/s80_20/epoc32
mkdir -p "$HERE/bin"
sed "s|EPOC32|$E|g" "$HERE/sis/pomocnik.pkg.in" > "$HERE/bin/pomocnik.pkg"
cd "$HERE/bin" && makesis pomocnik.pkg pomocnik.sis && rm pomocnik.pkg
ls -l "$HERE/bin/pomocnik.sis"
