add_library(Net STATIC
	BitStream.cpp
	Demo.cpp
	Fesl.cpp
	GameClient.cpp
	GameConnection.cpp
	EventChannel.cpp
	GameEvents.cpp
	GameProtocol.cpp
	GameServer.cpp
	GhostLayout.cpp
	Networkables.cpp
	RemoteConsole.cpp
	RemoteWorld.cpp
	ServerSettings.cpp
	Md5.cpp
	GameSpyCrypt.cpp
	ServerBrowser.cpp
	ServerQuery.cpp
	Socket.cpp
	TlsSocket.cpp
)
target_include_directories(Net PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Net PUBLIC World PRIVATE zlibstatic)
if(WIN32)
	target_link_libraries(Net PUBLIC ws2_32)
endif()

# TLS for the account service (FESL). Optional: without OpenSSL the game
# cannot log in but everything else builds. Homebrew keeps it out of the
# default search path on macOS.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
	foreach(prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
		if(EXISTS ${prefix})
			set(OPENSSL_ROOT_DIR ${prefix})
			break()
		endif()
	endforeach()
endif()
# A cross build would find the host's libraries: it takes OpenSSL only
# from an OPENSSL_ROOT_DIR given for the target.
if(NOT CMAKE_CROSSCOMPILING OR OPENSSL_ROOT_DIR)
	find_package(OpenSSL 1.1)
endif()
if(OpenSSL_FOUND)
	target_link_libraries(Net PRIVATE OpenSSL::SSL OpenSSL::Crypto)
	target_compile_definitions(Net PRIVATE BF2142_HAVE_TLS)
else()
	message(STATUS "OpenSSL not found: building without the account service (FESL) login")
endif()
