#!/usr/bin/env sh
set -euo pipefail

relpath() {
  # https://stackoverflow.com/a/12498485
  local target=$1
  local base=$2
  local up=''
  while [ -n "$base" ] && [ "${target#$base}" = "$target" ]; do
    base=$(dirname "$base")
    up="../$up"
  done
  echo "${up}${target#$base/}"
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SCRIPT="$ROOT_DIR/scripts/sh/cmake/new_module.sh"

if [ ! -x "$SCRIPT" ]; then
  echo "❌ Erreur : $SCRIPT n'existe pas ou n'est pas exécutable"
  exit 1
fi

echo "🔁 Mise à jour des CMakeLists.txt pour tous les modules..."

for base in "$ROOT_DIR"/libs "$ROOT_DIR"/src; do
  for module_path in "$base"/*; do
    if [ -d "$module_path/src" ]; then
      module_name="$(basename "$module_path")"
      relative_path=$(relpath "$module_path" "$ROOT_DIR")
      echo "⚙️  Mise à jour : $relative_path"
      "$SCRIPT" "$module_name" "$relative_path"
    fi
  done
done

echo "✅ Tous les CMakeLists.txt ont été régénérés."
