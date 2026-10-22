# Third-party libraries, built from source so every target (native and the
# 32-bit retail-compat build) uses the same versions.
include(FetchContent)

# ogg/vorbis declare an ancient cmake_minimum_required that CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(ogg
	GIT_REPOSITORY https://github.com/xiph/ogg.git
	GIT_TAG v1.3.5
	GIT_SHALLOW TRUE)
FetchContent_Declare(vorbis
	GIT_REPOSITORY https://github.com/xiph/vorbis.git
	GIT_TAG v1.3.7
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ogg)
# vorbis looks for an installed Ogg; point it at the one we just built.
set(OGG_INCLUDE_DIR "${ogg_SOURCE_DIR}/include;${ogg_BINARY_DIR}/include" CACHE PATH "" FORCE)
set(OGG_LIBRARY ogg CACHE STRING "" FORCE)
FetchContent_MakeAvailable(vorbis)

# zlib for ZIP archives (the retail game shipped zlib 1.2.2 as zlib122.dll).
FetchContent_Declare(zlib
	GIT_REPOSITORY https://github.com/madler/zlib.git
	GIT_TAG v1.3.1
	GIT_SHALLOW TRUE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)
# zlib's CMake defines both "zlib" (shared) and "zlibstatic"; we link statically.
target_include_directories(zlibstatic PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

# Python 3 for the game's scripts (the retail game embeds Python 2.3 as
# dice_py.dll). Optional; not used by the retail-compat build.
if(NOT BF2142_RETAIL_COMPAT AND NOT CMAKE_CROSSCOMPILING)
	find_package(Python3 3.9 COMPONENTS Development.Embed)
endif()

# Speex for in-game voice chat. BF2142.exe links Speex's narrowband codec
# statically (through GameSpy Voice 2); the rewrite builds the official
# sources (BSD licence). Speex's own build is autotools, so the library's
# sources are listed here.
FetchContent_Declare(speex
	GIT_REPOSITORY https://gitlab.xiph.org/xiph/speex.git
	GIT_TAG Speex-1.2.1
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(speex)
set(BF2142_SPEEX_SOURCES bits cb_search exc_10_16_table exc_10_32_table exc_20_32_table exc_5_256_table
	exc_5_64_table exc_8_128_table filters gain_table gain_table_lbr hexc_10_32_table hexc_table high_lsp_tables
	lpc lsp lsp_tables_nb ltp modes modes_wb nb_celp quant_lsp sb_celp speex speex_callbacks speex_header
	stereo vbr vq window)
list(TRANSFORM BF2142_SPEEX_SOURCES PREPEND ${speex_SOURCE_DIR}/libspeex/)
list(TRANSFORM BF2142_SPEEX_SOURCES APPEND .c)
add_library(speex STATIC ${BF2142_SPEEX_SOURCES})
set(BF2142_SPEEX_CONFIG ${CMAKE_BINARY_DIR}/speex-config)
file(WRITE ${BF2142_SPEEX_CONFIG}/speex_config_types.h
	"#pragma once\n#include <stdint.h>\ntypedef int16_t spx_int16_t;\ntypedef uint16_t spx_uint16_t;\n"
	"typedef int32_t spx_int32_t;\ntypedef uint32_t spx_uint32_t;\n")
file(WRITE ${BF2142_SPEEX_CONFIG}/config.h
	"#define FLOATING_POINT\n#define EXPORT\n#ifdef _MSC_VER\n#define USE_ALLOCA\n#else\n#define VAR_ARRAYS\n#endif\n")
target_compile_definitions(speex PRIVATE HAVE_CONFIG_H)
target_include_directories(speex PUBLIC ${speex_SOURCE_DIR}/include ${BF2142_SPEEX_CONFIG}
	PRIVATE ${speex_SOURCE_DIR}/libspeex)
if(MSVC)
	target_compile_options(speex PRIVATE /w)
else()
	target_compile_options(speex PRIVATE -w)
endif()
if(UNIX)
	target_link_libraries(speex PRIVATE m)
endif()
