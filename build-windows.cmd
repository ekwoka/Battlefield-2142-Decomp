@echo off
setlocal
set "ROOT=%~dp0"
set "TOOLS=%ROOT%..\llvm-mingw\llvm-mingw-20261117-ucrt-x86_64"
set "CMAKE=%ROOT%..\cmake-portable\cmake-4.4.4-windows-x86_64\bin\cmake.exe"
set "NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "SDL=%ROOT%..\SDL3\SDL3-3.4.16\x86_64-w64-mingw32"
set "OPENAL=%ROOT%..\openal-soft\openal-soft-1.25.2-bin\bin\Win64\soft_oal.dll"
set "SHADERS=%ROOT%..\shader-tools\bin"
set "DRIVE=Q:"
subst %DRIVE% "%ROOT%" >nul 2>&1
if errorlevel 1 (
	echo Could not map Q: to the checkout. Close any existing Q: mapping and retry.
	exit /b 1
)
"%CMAKE%" -S Q:\ -B Q:\build\mingw -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_C_COMPILER="%TOOLS%\bin\clang.exe" -DCMAKE_CXX_COMPILER="%TOOLS%\bin\clang++.exe" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" -DPython3_EXECUTABLE=C:/Python313/python.exe -DPython3_ROOT_DIR=C:/Python313 -DPython3_INCLUDE_DIR=C:/Python313/include -DPython3_LIBRARY=C:/Python313/libs/python313.lib -DCMAKE_REQUIRE_FIND_PACKAGE_Python3=ON -DSDL3_DIR="%SDL%\lib\cmake\SDL3" -DGLSLANG_VALIDATOR="%SHADERS%\glslangValidator.exe" -DSPIRV_CROSS="%SHADERS%\spirv-cross.exe" -DBF2142_STEAM_AUDIO=ON
if errorlevel 1 goto failed
set "PATH=%TOOLS%\bin;%PATH%"
"%NINJA%" -C Q:\build\mingw bf2142
if errorlevel 1 goto failed
copy /Y "%SDL%\bin\SDL3.dll" Q:\build\mingw\src\Main\SDL3.dll >nul
if errorlevel 1 goto failed
copy /Y "%OPENAL%" Q:\build\mingw\src\Main\OpenAL32.dll >nul
if errorlevel 1 goto failed
copy /Y "C:\Python313\python313.dll" Q:\build\mingw\src\Main\ >nul
if errorlevel 1 goto failed
copy /Y "C:\Python313\vcruntime140*.dll" Q:\build\mingw\src\Main\ >nul
if errorlevel 1 goto failed
subst %DRIVE% /d
exit /b %errorlevel%

:failed
set "RESULT=%errorlevel%"
subst %DRIVE% /d
exit /b %RESULT%
