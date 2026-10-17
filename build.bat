@echo off
rem build.bat - FractalStream build for Windows with MSVC (cl.exe).
rem Run it from a "x64 Native Tools Command Prompt for VS" (Visual Studio 2019 16.8 or newer).
rem
rem   build.bat              optimized build          -> build\fractalstream.exe
rem   build.bat debug        debug build              -> build\fractalstream_debug.exe
rem   build.bat test [filt]  build and run the tests  -> build\tests.exe
rem
rem GLFW is compiled once into build\glfw\*.obj; delete that folder to recompile it.
rem Mirrors build.sh: keep the GLFW file lists in sync with it.
setlocal enabledelayedexpansion
cd /d "%~dp0"
set MODE=%~1
if "%MODE%"=="" set MODE=release

where cl >nul 2>nul
if errorlevel 1 (
  echo build.bat: cl.exe not found. Open a "x64 Native Tools Command Prompt for VS" and try again.
  exit /b 1
)

set GLFW_SRC=context init input monitor platform vulkan window egl_context osmesa_context null_init null_monitor null_window null_joystick win32_module win32_time win32_thread win32_init win32_joystick win32_monitor win32_window wgl_context
if not exist build\glfw mkdir build\glfw
set OBJS=
for %%f in (%GLFW_SRC%) do (
  if not exist build\glfw\%%f.obj (
    cl /nologo /c /O2 /D_GLFW_WIN32 /D_CRT_SECURE_NO_WARNINGS third_party\glfw\src\%%f.c /Fobuild\glfw\%%f.obj
    if errorlevel 1 exit /b 1
  )
  set OBJS=!OBJS! build\glfw\%%f.obj
)

set INC=/Ithird_party\vulkan /Ithird_party\glfw\include /Ithird_party\stb
set CFLAGS=/nologo /std:c11 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS
set LIBS=gdi32.lib user32.lib shell32.lib
set MAIN=src\main.c
set EXE=
if /i "%MODE%"=="release" set FLAGS=/O2& set EXE=fractalstream.exe
if /i "%MODE%"=="debug" set FLAGS=/Od /Zi /RTC1& set EXE=fractalstream_debug.exe
if /i "%MODE%"=="test" set FLAGS=/O1 /Zi& set EXE=tests.exe& set MAIN=tests\test_main.c
if "%EXE%"=="" (
  echo build.bat: unknown mode "%MODE%" ^(use release, debug or test^)
  exit /b 1
)

cl %CFLAGS% %FLAGS% %INC% %MAIN% %OBJS% /Fobuild\ /Fdbuild\ /Febuild\%EXE% /link %LIBS%
if errorlevel 1 exit /b 1
echo built build\%EXE%

if /i "%MODE%"=="test" (
  build\%EXE% %2
  exit /b !errorlevel!
)
