#!/bin/sh
# Builds the arm64 .deb in a Debian 13 (trixie) container. Run from anywhere. Output: apps/midi-scope/out/
# Source goes in and results come out as tar streams (a Docker VM such as colima cannot see the host volume).
set -e
cd "$(dirname "$0")/.."
docker build -q --platform linux/arm64 -t midiscope-build -f tools/Dockerfile tools >/dev/null 2>&1
rm -rf out; mkdir -p out
tar --exclude=build -cf - CMakeLists.txt LICENSE src tools/testsrc.cpp tests third_party packaging | \
docker run --rm -i --platform linux/arm64 midiscope-build sh -c '
  mkdir /src && cd /src && tar xf - && cmake -S . -B build >/dev/null && cmake --build build -j4 2>&1 | grep -E "warning|error" >&2 || true
  cd build && ctest --output-on-failure >&2
  mkdir /res && for v in "" "tab" "tab,tab"; do ./midi-scope --ascii --demo --ms 100 --keys "${v:-up}" ; done > /res/demo.txt; ./midi-scope --ascii --demo --ms 100 --ppm /res/demo.ppm >/dev/null
  cpack -G DEB >/dev/null && cp midi-scope_*.deb /res/ && dpkg-deb -c /res/midi-scope_*.deb >&2
  tar -C /res -cf - .' | tar -C out -xf -
ls -la out
