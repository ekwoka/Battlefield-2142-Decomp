add_library(Texture STATIC
	DdsFile.cpp
	TgaFile.cpp
)
target_include_directories(Texture PUBLIC ${PROJECT_SOURCE_DIR}/src)
