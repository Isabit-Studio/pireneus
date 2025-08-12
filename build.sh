#!/bin/bash

# Détection OS et arch
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=${1:-$(uname -m)}  # Arch par défaut ou arg (e.g., amd64, arm64)
if [[ "$OS" == "mingw"* || "$OS" == "cygwin"* || "$OS" == "msys"* ]]; then
    OS="windows"
fi
TARGET="$ARCH-$OS"  # e.g., amd64-linux

# Compilateurs et flags
if [[ "$OS" == "windows" ]]; then
    CC="cl.exe"  # MSVC
    CFLAGS="/MT /O2 /W3"  # Statique, optimisé
    OBJ_EXT=".obj"
    LIB_EXT=".lib"
    EXE_EXT=".exe"
    LIB_CMD="lib"  # Pour créer .lib
    LINK_FLAGS="/link /OUT:bin/$TARGET/game_kernel$EXE_EXT"
else
    CC="clang"  # Ou gcc si préféré: CC="gcc"
    CFLAGS="-O2 -Wall -static"  # Statique
    OBJ_EXT=".o"
    LIB_EXT=".a"
    EXE_EXT=""
    LIB_CMD="ar rcs"  # Pour créer .a
    LINK_FLAGS="-o bin/$TARGET/game_kernel$EXE_EXT"
fi

# Dossiers
SRC_DIR="src"
OBJ_DIR="obj/$TARGET"
BIN_DIR="bin/$TARGET"
LIB_DIR="lib/$TARGET"
INCLUDE_DIR="sys/include"

mkdir -p $OBJ_DIR $BIN_DIR $LIB_DIR

# Compiler les sources en objets
compile() {
    local src=$1
    local obj="$OBJ_DIR/$(basename ${src%.*})$OBJ_EXT"
    if [[ "$OS" == "windows" ]]; then
        $CC $CFLAGS /c $src /Fo$obj /I$INCLUDE_DIR
    else
        $CC $CFLAGS -c $src -o $obj -I$INCLUDE_DIR
    fi
}

# Compiler noyau
compile $SRC_DIR/cmd/kernel.c

# Compiler modules et créer libs
for module_dir in $SRC_DIR/lib/*; do
    module_name=$(basename $module_dir)
    for src in $module_dir/*.c; do
        compile $src
    done
    # Créer lib statique pour le module
    module_lib="$LIB_DIR/lib$module_name$LIB_EXT"
    objs=$(find $OBJ_DIR -name "${module_name}_*$OBJ_EXT")
    $LIB_CMD $module_lib $objs
done

# Compiler code port si applicable
if [ -d "$SRC_DIR/port/$OS" ]; then
    for src in $SRC_DIR/port/$OS/*.c; do
        compile $src
    done
fi

# Linker tout (noyau + libs modules + ports)
all_objs=$(find $OBJ_DIR -name "*$OBJ_EXT" | grep -v "module")  # Objets non-modules
all_libs=$(find $LIB_DIR -name "*$LIB_EXT")
if [[ "$OS" == "windows" ]]; then
    $CC $all_objs $all_libs $CFLAGS $LINK_FLAGS
else
    $CC $all_objs $all_libs $CFLAGS $LINK_FLAGS
fi

echo "Build terminé pour $TARGET dans bin/$TARGET/"
