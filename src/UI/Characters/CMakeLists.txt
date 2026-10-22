add_library(SwfCharacters STATIC
	Bitmap.cpp
	Button.cpp
	CharacterDictionary.cpp
	EditText.cpp
	Font.cpp
	Shape.cpp
	Tessellate.cpp
)
target_include_directories(SwfCharacters PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(SwfCharacters PUBLIC UI PRIVATE zlibstatic)
