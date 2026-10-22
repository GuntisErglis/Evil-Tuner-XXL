#!/usr/bin/env bash
# Build the engine for this computer (x86) with the test host, and the helpers it starts (et_exec; ffmpeg is the
# system's). ASan + UBSan.   test/build_host.sh  ->  build/host/host, build/host/plugin/bin/
set -euo pipefail
cd "$(dirname "$0")/.."
. ./build_srcs.sh
W=third_party/mpc-vst-plugins/wrapper
python3 tools/gen_params.py >/dev/null
mkdir -p build/host/obj build/host/plugin/bin
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
OBJS=""
for f in $ENGINE_SRCS; do o=build/host/obj/$(echo $f | tr / _).o; g++ -std=gnu++17 -O1 -g $SAN -DET_TEST $DEFS $INCS -I$W -c $f -o $o; OBJS="$OBJS $o"; done
for f in $DF_SRCS $CL_SRCS; do o=build/host/obj/$(echo $f | tr / _).o; [ -f $o ] || g++ -std=gnu++11 -O2 -g -w $DEFS $INCS -c $f -o $o; OBJS="$OBJS $o"; done
g++ -std=gnu++17 -O1 -g $SAN -I$W test/host_main.cc $OBJS -ldl -lpthread -o build/host/host
gcc -O2 helpers/et_exec.c -o build/host/plugin/bin/et_exec
ln -sf "$(command -v ffmpeg)" build/host/plugin/bin/ffmpeg
echo "-> build/host/host"
