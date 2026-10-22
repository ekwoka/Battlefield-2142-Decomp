# Voice chat: the protocol, Speex, the client, the voice server
# (BF2VoipServer.dll) and the game server's connection to it.
add_library(Voip STATIC
	VoipProtocol.cpp
	VoipCodec.cpp
	VoipServer.cpp
	VoipServerConnection.cpp
	VoipClient.cpp
	VoiceChat.cpp
)
target_include_directories(Voip PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Voip PUBLIC Net PRIVATE speex)

# Microphone and speakers through SDL3 when the build has it (as the
# renderer); without it voice runs without audio devices.
if(NOT CMAKE_CROSSCOMPILING)
	find_package(SDL3 CONFIG QUIET)
endif()
if(SDL3_FOUND)
	target_sources(Voip PRIVATE SdlVoiceDevice.cpp)
	target_link_libraries(Voip PRIVATE SDL3::SDL3)
else()
	target_sources(Voip PRIVATE NoVoiceDevice.cpp)
endif()

# The menu's dice.bf2.Sound.voip* natives.
add_library(VoipMenu STATIC VoipNatives.cpp)
target_link_libraries(VoipMenu PUBLIC Voip SwfPlayer)
