# The Bink movies (the start-up logos and intro, bf2Engine.playMovie)
# through FFmpeg's Bink decoders. Optional: BF2142_MOVIES=OFF, or no FFmpeg
# found (brew install ffmpeg; libavformat, libavcodec, libswscale,
# libswresample through pkg-config), builds the queue without a decoder
# and the movies are skipped. A cross build (the 32-bit Windows one) takes
# FFmpeg only from a PKG_CONFIG_PATH given for the target.
option(BF2142_MOVIES "Play the Bink movies (needs FFmpeg)" ON)

add_library(Movie STATIC MovieQueue.cpp MovieDecoder.cpp MenuVideo.cpp)
target_include_directories(Movie PUBLIC ${PROJECT_SOURCE_DIR}/src)

if(BF2142_MOVIES AND (NOT CMAKE_CROSSCOMPILING OR DEFINED ENV{PKG_CONFIG_PATH}))
	find_package(PkgConfig QUIET)
	if(PKG_CONFIG_FOUND)
		pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
	endif()
endif()
if(FFMPEG_FOUND)
	target_link_libraries(Movie PRIVATE PkgConfig::FFMPEG)
	target_compile_definitions(Movie PUBLIC BF2142_HAS_MOVIES=1)
	message(STATUS "Movies: FFmpeg ${FFMPEG_libavcodec_VERSION} (Bink video and audio)")
else()
	message(STATUS "Movies: no FFmpeg (or BF2142_MOVIES off): the Bink movies are skipped")
endif()
