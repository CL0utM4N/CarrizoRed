#!/bin/bash
#
#  Fetches the build dependencies into the project root (macOS only):
#    - Lilu.kext (debug build, provides the plugin SDK headers + plugin_start.cpp)
#    - MacKernelSDK
#
#  Re-run with --force to replace existing copies.
#

set -euo pipefail

cd "$(dirname "$0")/.."

if [ "${1:-}" = "--force" ]; then
  rm -rf Lilu.kext MacKernelSDK
fi

if [ ! -d Lilu.kext ]; then
  # Lilu's own bootstrap builds a debug Lilu.kext into the current directory. Run it in a
  # separate shell: it reads unset variables (TRAVIS_TAG, BUILD_MODE), which `set -u` rejects.
  src=$(/usr/bin/curl -Lfs https://raw.githubusercontent.com/acidanthera/Lilu/master/Lilu/Scripts/bootstrap.sh)
  /bin/bash -c "$src"
fi

if [ ! -d MacKernelSDK ]; then
  git clone --depth 1 https://github.com/acidanthera/MacKernelSDK.git
fi

echo "Dependencies ready. Run 'make' or 'make DEBUG=1'."
