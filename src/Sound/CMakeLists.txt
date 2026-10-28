add_library(SoundEngine STATIC
	SoundEngine/Ambience.cpp
	SoundEngine/BfOpenAL.cpp
	SoundEngine/BfSoundEngine.cpp
	SoundEngine/Music.cpp
	SoundEngine/OggStream.cpp
	SoundEngine/OpenALProvider.cpp
	SoundEngine/OpenALSample.cpp
	SoundEngine/Reverb.cpp
	SoundEngine/Sound.cpp
	SoundEngine/SoundEnvelope.cpp
	SoundEngine/SoundFile.cpp
	SoundEngine/SoundServices.cpp
	SoundEngine/SoundTemplate.cpp
	SoundEngine/SoundTweaks.cpp
	SoundEngine/WavFileFormat.cpp
)
target_include_directories(SoundEngine PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(SoundEngine PUBLIC Platform PRIVATE vorbisfile vorbis ogg)
find_package(Threads REQUIRED)
target_link_libraries(SoundEngine PRIVATE Threads::Threads)
if(NOT WIN32)
	target_link_libraries(SoundEngine PRIVATE ${CMAKE_DL_LIBS})
endif()

if(BF2142_RETAIL_COMPAT)
	# Drop-in BF2142Audio.dll for the retail game.
	add_library(BF2142AudioDll SHARED retail/BF2142AudioDll.cpp)
	set_target_properties(BF2142AudioDll PROPERTIES OUTPUT_NAME BF2142Audio PREFIX "")
	target_link_libraries(BF2142AudioDll PRIVATE SoundEngine)
	target_link_options(BF2142AudioDll PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--kill-at)
endif()

# The executable's side of sound: sound templates, the sound manager and the
# "Sound.*" console methods.
add_library(GameSound STATIC
	Game/GameSounds.cpp
	Game/SoundConsole.cpp
	Game/SoundManager.cpp
	Game/SoundObjectTemplate.cpp
	Game/VfsSoundFileSystem.cpp
	Game/VoiceMessages.cpp
)
target_include_directories(GameSound PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(GameSound PUBLIC SoundEngine World Console IO)
