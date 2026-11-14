#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <stdio.h>

// DEBUG_LOG compiles away entirely outside of debug builds. DEBUG is defined project-wide for
// the CMake "Debug" configuration (see CMakeLists.txt), so nothing needs to be defined manually.
#ifdef DEBUG
#define DEBUG_LOG(...) printf(__VA_ARGS__)
#else
#define DEBUG_LOG(...)
#endif

#endif // _DEBUG_H_
