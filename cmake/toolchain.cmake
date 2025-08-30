# =============================================================================
# Toolchain file for using clang and adapting to the platform
# =============================================================================

# Détecte automatiquement clang
set(CMAKE_C_COMPILER clang CACHE STRING "C compiler" FORCE)

# Configure CMake selon la plateforme détectée
if(APPLE)
    set(CMAKE_SYSTEM_NAME Darwin)
    set(CMAKE_GENERATOR "Xcode")
elseif(UNIX)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_GENERATOR "Unix Makefiles")
else()
    message(FATAL_ERROR "Unsupported platform for this toolchain")
endif()

# Forcer le comportement si nécessaire
set(CMAKE_C_COMPILER_FORCED TRUE CACHE INTERNAL "")
