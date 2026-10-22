add_library(IO STATIC
	File/FileManager.cpp
	File/ZipArchive.cpp
	Localization/Localization.cpp
)
target_include_directories(IO PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(IO PRIVATE zlibstatic)
