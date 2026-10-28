# Shaders are written once in GLSL (Vulkan dialect) and compiled at build
# time to SPIR-V (Vulkan) with glslangValidator and to Metal Shading Language
# with spirv-cross, the same route SDL_shadercross takes. Metal resource
# indices follow the GLSL binding numbers (--msl-decoration-binding), which
# is how SDL binds them; by default spirv-cross numbers them by first use. Both are embedded
# into the target as byte arrays in EmbeddedShaders.h.
find_program(GLSLANG_VALIDATOR glslangValidator)
find_program(SPIRV_CROSS spirv-cross)

function(bf2142_embed_shaders target)
	if(NOT GLSLANG_VALIDATOR OR NOT SPIRV_CROSS)
		message(FATAL_ERROR "glslangValidator and spirv-cross are needed to build ${target} (brew install glslang spirv-cross)")
	endif()
	set(dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
	set(outputs)
	# Shared GLSL (#include "x.glsl" beside the shaders) rebuilds them all.
	file(GLOB includes ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.glsl)
	foreach(source ${ARGN})
		get_filename_component(name ${source} NAME)
		set(spv ${dir}/${name}.spv)
		set(msl ${dir}/${name}.msl)
		add_custom_command(OUTPUT ${spv} ${msl}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${dir}
			COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.0 --quiet -o ${spv} ${CMAKE_CURRENT_SOURCE_DIR}/${source}
			COMMAND ${SPIRV_CROSS} --msl --msl-version 20100 --msl-decoration-binding --output ${msl} ${spv}
			DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source} ${includes}
			COMMENT "Compiling shader ${name}"
			VERBATIM)
		list(APPEND outputs ${spv} ${msl})
	endforeach()
	string(REPLACE ";" "|" inputs "${outputs}")
	set(header ${dir}/EmbeddedShaders.h)
	add_custom_command(OUTPUT ${header}
		COMMAND ${CMAKE_COMMAND} -DOUTPUT=${header} "-DINPUTS=${inputs}" -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
		DEPENDS ${outputs} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
		COMMENT "Embedding shaders"
		VERBATIM)
	target_sources(${target} PRIVATE ${header})
	target_include_directories(${target} PRIVATE ${dir})
endfunction()
