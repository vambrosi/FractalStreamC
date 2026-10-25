@echo off
rem build.bat - FractalStreamC build for Windows with MSVC (cl.exe).
rem Run it from a "x64 Native Tools Command Prompt for VS" (Visual Studio 2019 16.8 or newer).
rem
rem   build.bat              optimized build          -> build\fractalstreamc.exe
rem   build.bat debug        debug build              -> build\fractalstreamc_debug.exe
rem   build.bat test [filt]  build and run the tests  -> build\tests.exe
rem
rem GLFW is compiled once into build\glfw\*.obj, and the video encoder and MP4 writer into build\video\*.obj;
rem delete those folders to recompile them. Mirrors build.sh: keep the GLFW file lists in sync with it.
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

rem the video encoder and MP4 writer (third_party\minih264, third_party\minimp4: public domain)
if not exist build\video mkdir build\video
for %%f in (minih264\minih264e minimp4\minimp4) do (
  if not exist build\video\%%~nf.obj (
    cl /nologo /c /O2 /D_CRT_SECURE_NO_WARNINGS third_party\%%f.c /Fobuild\video\%%~nf.obj
    if errorlevel 1 exit /b 1
  )
  set OBJS=!OBJS! build\video\%%~nf.obj
)

set INC=/Ithird_party\vulkan /Ithird_party\glfw\include /Ithird_party\stb /Ithird_party\minih264 /Ithird_party\minimp4
set CFLAGS=/nologo /std:c11 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS
set LIBS=gdi32.lib user32.lib shell32.lib
set MAIN=src\main.c
set EXE=
if /i "%MODE%"=="release" set FLAGS=/O2& set EXE=fractalstreamc.exe
if /i "%MODE%"=="debug" set FLAGS=/Od /Zi /RTC1& set EXE=fractalstreamc_debug.exe
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
