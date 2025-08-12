# Pireneus



## architecture proposal :

    - inspired by plan9 ( TODO ) 

```sh

pireneus/
├── bin/                  # Final binaries (by arch/platform)
│   ├── amd64-linux/      # For Linux x86_64
│   │   └── game_kernel   # Static executable
│   ├── amd64-windows/    # For Windows x86_64
│   │   └── game_kernel.exe
│   └── arm64-macos/      # For macOS ARM
│       └── game_kernel
├── lib/                  # Static module libraries (by arch)
│   ├── amd64-linux/
│   │   ├── libgraphics.a
│   │   └── libphysics.a
│   ├── amd64-windows/
│   │   ├── graphics.lib
│   │   └── physics.lib
│   └── arm64-macos/
│       ├── libgraphics.a
│       └── libphysics.a
├── obj/                  # Temporary objects (.o or .obj, by arch - inspired by Plan 9 builds)
│   ├── amd64-linux/
│   │   ├── kernel.o
│   │   ├── graphics_module.o
│   │   └── physics_module.o
│   ├── amd64-windows/
│   │   ├── kernel.obj
│   │   ├── graphics_module.obj
│   │   └── physics_module.obj
│   └── arm64-macos/
│       ├── kernel.o
│       ├── graphics_module.o
│       └── physics_module.o
├── sys/                  # Global documentation and includes (like /sys in Plan 9)
│   ├── doc/              # Documentation
│   │   └── README.md     # Module explanations
│   └── include/          # Common headers
│       ├── kernel.h      # Kernel API
│       └── module.h      # Module interface
├── src/                  # Main sources (like /sys/src in Plan 9)
│   ├── cmd/              # Main kernel code (inspired by /sys/src/cmd)
│   │   └── kernel.c      # Kernel that loads modules
│   ├── lib/              # Module sources (inspired by /sys/src/lib)
│   │   ├── graphics/     # Graphics module
│   │   │   └── graphics_module.c
│   │   └── physics/      # Physics module
│   │       └── physics_module.c
│   └── port/             # Architecture/OS specific code (for portability, like in Plan 9)
│       ├── windows/      # Implemented for Windows/MSVC
│       │   └── win_port.c
│       ├── linux/        # For Linux/Clang
│       │   └── linux_port.c
│       └── macos/        # For macOS/Clang
│           └── macos_port.c
├── build.sh              # Build script (run with ./build.sh)
└── clean.sh              # Clean script





```

