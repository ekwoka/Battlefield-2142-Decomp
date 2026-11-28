cmake_minimum_required(VERSION 3.24)
project(bf2142 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# BF2142_RETAIL_COMPAT builds 32-bit Windows drop-in replacements for the
# retail DLLs (same file names and exports), for testing against the original
# BF2142.exe. Native builds produce the portable game instead.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
	set(BF2142_RETAIL_COMPAT_DEFAULT ON)
else()
	set(BF2142_RETAIL_COMPAT_DEFAULT OFF)
endif()
option(BF2142_RETAIL_COMPAT "Build drop-in replacements for the retail DLLs" ${BF2142_RETAIL_COMPAT_DEFAULT})

# No fused multiply-adds: clang contracts a * b + c into one rounding by
# default (-ffp-contract=on), and does so where the target has FMA (arm64
# always, x86-64 only with -march flags), so the same round would compute
# different floats on Apple Silicon and on x86-64. Off, every target rounds
# each operation, and the simulation (demos, state fingerprints) matches
# across machines. MSVC's /fp:precise doesn't contract either.
if(MSVC)
	add_compile_options(/W4 /fp:precise)
else()
	add_compile_options(-Wall -Wextra -ffp-contract=off)
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
