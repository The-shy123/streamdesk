#!/usr/bin/env bash
# No installations, remote connections, uploads, or server dependencies.
set -euo pipefail
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"
mkdir -p build/verification
{
  uname -srm
  if [ -f /etc/os-release ]; then cat /etc/os-release; fi
  if command -v sw_vers >/dev/null; then sw_vers; fi
  c++ --version
  cmake --version
  ctest --version
  make --version
} > build/verification/core-environment.log 2>&1
python3 tools/source_manifest.py > build/verification/core-source.sha256
for mode in Debug Release; do
  out="build/core-$mode"
  cmake -S . -B "$out" -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE="$mode" -DBUILD_TESTING=ON -DSTREAMDESK_BUILD_SERVER=OFF 2>&1 | tee "build/verification/core-$mode-configure.log"
  cmake --build "$out" --parallel 2 2>&1 | tee "build/verification/core-$mode-build.log"
  "$out/streamcore_example" 2>&1 | tee "build/verification/core-$mode-example.log"
  ctest --test-dir "$out" -N 2>&1 | tee "build/verification/core-$mode-list.log"
  ctest --test-dir "$out" --output-on-failure --no-tests=error 2>&1 | tee "build/verification/core-$mode-test.log"
done
printf 'Core Debug/Release verification finished successfully.\n'
