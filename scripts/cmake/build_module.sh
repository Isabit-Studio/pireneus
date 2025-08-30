#!/bin/sh
set -euo pipefail

# Fonction pour calculer un chemin relatif (POSIX only, pas de realpath)
relpath() {
  local target=$1
  local base=$2
  local up=''
  while [ -n "$base" ] && [ "${target#$base}" = "$target" ]; do
    base=$(dirname "$base")
    up="../$up"
  done
  echo "${up}${target#$base/}"
}

if [ $# -ne 1 ]; then
  echo "Usage: $0 <module_path>" >&2
  exit 1
fi

# Calcule le chemin relatif depuis ROOT_DIR
ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
MODULE_INPUT_PATH="$1"
MODULE_ABS_PATH="$(cd "$MODULE_INPUT_PATH" && pwd)"
MODULE_PATH=$(relpath "$MODULE_ABS_PATH" "$ROOT_DIR")
TOOLCHAIN_PATH="$ROOT_DIR/cmake/toolchain.cmake"

# Compilation du module
echo "📦 Building module at $MODULE_PATH"
cmake -S "$MODULE_PATH" -B "$MODULE_PATH/bin" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_PATH"
cmake --build "$MODULE_PATH/bin"

