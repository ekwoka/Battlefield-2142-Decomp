# Cross-compiles 32-bit Windows binaries that can be dropped into the original
# game install (e.g. a CrossOver/Wine bottle) to test rewritten modules against
# the retail executable.
#   cmake -B build/win32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
