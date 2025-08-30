# Pireneus

Pireneus is a modular, flexible and evolutionary game engine aimed at offering a good developer experience.


Please at your fist launch run `.scripts/sh/bootstrap.sh`

create a module : `scripts/sh/new_module.sh`

Warning : all modules are based on the same layout including the CMakeList.txt file. If you need to modify something please adapte the `scripts/sh/new_module.sh`

to build a module please use `scripts/sh/build_module.sh`

I will produce an explanation of the CMake configuration soon.

### TODO :

- review Cmake and create a toolchain to produce an executable (inspiration : golang ? )

- see u.h system from plan9