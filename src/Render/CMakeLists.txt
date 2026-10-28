# The renderer needs SDL3 (brew install sdl3) and the shader tools
# (brew install glslang spirv-cross); without SDL3 it is skipped.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
	message(STATUS "SDL3 not found: building without the renderer")
	return()
endif()
include(${PROJECT_SOURCE_DIR}/cmake/Shaders.cmake)

add_library(Render STATIC
	GpuContext.cpp
	LevelRenderer.cpp
	LevelRendererPost.cpp
	PngWriter.cpp
)
target_include_directories(Render PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Render PUBLIC SDL3::SDL3 Geometry Texture Terrain Scene World Animation Effects Hud IO PRIVATE zlibstatic)
bf2142_embed_shaders(Render
	Shaders/mesh.vert
	Shaders/mesh.frag
	Shaders/terrain.vert
	Shaders/terrain.frag
	Shaders/surrounding.frag
	Shaders/water.vert
	Shaders/water.frag
	Shaders/road.vert
	Shaders/road.frag
	Shaders/decal.vert
	Shaders/decal.frag
	Shaders/undergrowth.vert
	Shaders/undergrowth.frag
	Shaders/particle.vert
	Shaders/particle.frag
	Shaders/ui.vert
	Shaders/ui.frag
	Shaders/shadowcaster.frag
	Shaders/camo.frag
	Shaders/sky.frag
	Shaders/fullscreen.vert
	Shaders/bloom.frag
	Shaders/post.frag
)
