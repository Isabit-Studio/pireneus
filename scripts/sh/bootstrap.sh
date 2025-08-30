#!/bin/sh
set -e

make_executable() {
  if [ -f "$1" ]; then
    chmod u+x "$1"
  fi
}

# Rendre exécutables les scripts nécessaires
make_executable scripts/sh/cmake/cmake-download.sh
make_executable scripts/sh/setup.sh

# Exécuter les scripts dans l’ordre
sh scripts/sh/cmake/cmake-download.sh
sh scripts/sh/setup.sh

git config --local include.path ../.gitconfig

echo "✅ Bootstrap completed (sh)"
