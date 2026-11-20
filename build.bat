@echo off
rem build.bat - MSVC build of LibreTune for Windows (run from a Developer Command Prompt).
rem Untested on this project's CI so far; build.sh with MinGW is the verified Windows path.
rem Usage: build.bat          -> build\libretune.exe
rem Console subsystem on purpose so --help/--version print; add /SUBSYSTEM:WINDOWS
rem to the link line below for a console-less release build.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
rem all universal sources + src\plat\win32 + src\gui\os\win32 (not the posix/unix/macos dirs)
dir /s /b src\*.c | findstr /v /i /c:"\\plat\\posix\\" /c:"\\gui\\os\\unix\\" /c:"\\gui\\os\\macos\\" > build\srcs.txt
cl /nologo /W4 /WX /TC /O2 /Isrc /D_CRT_SECURE_NO_WARNINGS /Fobuild\ /Fe:build\libretune.exe @build\srcs.txt /link ws2_32.lib advapi32.lib gdi32.lib user32.lib shell32.lib setupapi.lib winusb.lib
if errorlevel 1 exit /b 1
echo built build\libretune.exe
