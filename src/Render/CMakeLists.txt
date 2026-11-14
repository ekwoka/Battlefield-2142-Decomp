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
	Enhanced/LevelRendererEnhanced.cpp
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
	Shaders/roadmesh.vert
	Shaders/decal.vert
	Shaders/decal.frag
	Shaders/undergrowth.vert
	Shaders/undergrowth.frag
	Shaders/particle.vert
	Shaders/particle.frag
	Shaders/shimmer.frag
	Shaders/ui.vert
	Shaders/ui.frag
	Shaders/shadowcaster.frag
	Shaders/camo.frag
	Shaders/sky.frag
	Shaders/fullscreen.vert
	Shaders/bloom.frag
	Shaders/post.frag
	# The enhanced mode's (not retail; Enhanced/LevelRendererEnhanced.cpp):
	# its passes, and the lit shaders' variants (name=shader+edits).
	Shaders/Enhanced/depth.frag
	Shaders/Enhanced/gbuffer.frag
	Shaders/Enhanced/motion.vert
	Shaders/Enhanced/motion.frag
	Shaders/Enhanced/gtao.frag
	Shaders/Enhanced/aoblur.frag
	Shaders/Enhanced/taa.frag
	enh_mesh.frag=Shaders/mesh.frag+Shaders/Enhanced/mesh.frag.patch
	enh_terrain.frag=Shaders/terrain.frag+Shaders/Enhanced/terrain.frag.patch
	enh_road.frag=Shaders/road.frag+Shaders/Enhanced/road.frag.patch
	enh_undergrowth.frag=Shaders/undergrowth.frag+Shaders/Enhanced/undergrowth.frag.patch
)
