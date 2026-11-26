@echo off
setlocal
set "ROOT=%~dp0"
set "BF2142_GAME_DIR=C:\Program Files (x86)\Electronic Arts\Battlefield 2142"
set "PATH=%USERPROFILE%\openssl-4.1.1-ucrt\bin;%USERPROFILE%\msys64\ucrt64\bin;%ROOT%..\llvm-mingw\llvm-mingw-20261117-ucrt-x86_64\bin;%BF2142_GAME_DIR%;%PATH%"
set "SSL_CERT_FILE=%USERPROFILE%\msys64\ucrt64\etc\ssl\certs\ca-bundle.crt"
cd /d "%ROOT%"
"%ROOT%build\mingw\src\Main\bf2142.exe" --game "%BF2142_GAME_DIR%" %*
