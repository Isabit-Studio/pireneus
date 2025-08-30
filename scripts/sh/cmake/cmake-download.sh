#!/bin/bash
set -e

detect_platform() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"

  case "$os" in
    Linux)
      echo "linux-x86_64"
      ;;
    Darwin)
      echo "macos-universal"
      ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
      echo "windows-x86_64"
      ;;
    *)
      echo "unsupported"
      return 1
      ;;
  esac
}

if [ "$1" = "--print-bin" ]; then
  CMAKE_VERSION="${CMAKE_VERSION:=4.0.2}"
  PLATFORM="$(detect_platform)"
  if [ "$PLATFORM" = "macos-universal" ]; then
    echo "$PWD/tools/cmake/cmake-${CMAKE_VERSION}-${PLATFORM}/CMake.app/Contents/bin/cmake"
  else
    echo "$PWD/tools/cmake/cmake-${CMAKE_VERSION}-${PLATFORM}/bin/cmake"
  fi
  exit 0
fi

# Version configurable à l'exécution
CMAKE_VERSION="${CMAKE_VERSION:=4.0.2}"
PLATFORM="$(detect_platform)"
INSTALL_DIR="./tools/cmake"
ARCHIVE="cmake-${CMAKE_VERSION}-${PLATFORM}.tar.gz"
URL="https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/${ARCHIVE}"

mkdir -p "$INSTALL_DIR"
cd "$INSTALL_DIR"

if [ ! -d "cmake-${CMAKE_VERSION}-${PLATFORM}" ]; then
  echo "📦 Downloading CMake $CMAKE_VERSION for $PLATFORM..."
  curl -LO "$URL"
  tar -xzf "$ARCHIVE"
  rm "$ARCHIVE"
fi

if [ "$PLATFORM" = "macos-universal" ]; then
  BIN_PATH="$(pwd)/cmake-${CMAKE_VERSION}-${PLATFORM}/CMake.app/Contents/bin/cmake"
else
  BIN_PATH="$(pwd)/cmake-${CMAKE_VERSION}-${PLATFORM}/bin/cmake"
fi

if [ ! -x "$BIN_PATH" ]; then
  echo "❌ Error: CMake binary not found at $BIN_PATH"
  exit 1
fi


YELLOW='\033[33m'
RESET='\033[0m'

echo "✅ CMake $CMAKE_VERSION ready at:"
echo "   $BIN_PATH"
echo
echo -e "${YELLOW}👨‍🎓 note : if you are using this script alone (cmake-download.sh)${RESET}"
echo -e "${YELLOW}👨‍🎓 note : 👉 To use it in your session:${RESET}"
echo -e "${YELLOW}👨‍🎓 note :   export CMAKE_BIN=\"$BIN_PATH\"${RESET}"

