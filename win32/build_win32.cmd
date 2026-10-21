@echo off
rem win32\build_win32.cmd - build the Win32 launcher with both compilers
rem and both configurations:
rem   1) MinGW-w64 gcc (d:\my\run\mingw64)
rem   2) Microsoft Visual C++ 6.0 (d:\my\run\Visual C++ 6.0\VC98)
rem   debug   -> build\debug\   (gcc -g -O0 -D_DEBUG / cl /Zi /Od /DEBUG)
rem   release -> build\release\ (gcc -O2 -DNDEBUG   / cl /O2)
rem Each executable embeds the VERSIONINFO resource from appboot.rc
rem (version 0.1.1, kept in sync with ..\src\version.h).
rem Layout: shared core sources in ..\src, win32 sources here.
rem All sources are strict C89.
setlocal
set ROOT=%~dp0
set SRC=%ROOT%
set CORE=%ROOT%..\src
set OUT=%ROOT%build
set VCROOT=d:\my\run\Visual C++ 6.0\VC98
set VSROOT=d:\my\run\Visual C++ 6.0
set MINGW=d:\my\run\mingw64\bin

set SHARED="%CORE%\cfg.c" "%CORE%\topo.c" "%CORE%\log.c" "%CORE%\cli.c" "%CORE%\ipc.c" "%CORE%\ipc_win32.c" "%SRC%\win32_service.c" "%SRC%\win32_gui.c"
set LIBS=-ladvapi32 -luser32 -lgdi32

if not exist "%OUT%\debug"   mkdir "%OUT%\debug"
if not exist "%OUT%\release" mkdir "%OUT%\release"

echo ============ [1/4] MinGW gcc DEBUG ============
"%MINGW%\windres.exe" -i "%SRC%appboot.rc" -O coff -o "%OUT%\debug\appboot_res.o"
if errorlevel 1 (echo windres FAILED & exit /b 1)
"%MINGW%\gcc.exe" -std=c89 -pedantic -Wall -Wextra -g -O0 -D_DEBUG ^
    -I"%CORE%" -o "%OUT%\debug\startup-gcc.exe" ^
    %SHARED% "%OUT%\debug\appboot_res.o" %LIBS%
if errorlevel 1 (echo MinGW debug build FAILED & exit /b 1)

echo ============ [2/4] MinGW gcc RELEASE ============
"%MINGW%\windres.exe" -i "%SRC%appboot.rc" -O coff -o "%OUT%\release\appboot_res.o"
if errorlevel 1 (echo windres FAILED & exit /b 1)
"%MINGW%\gcc.exe" -std=c89 -pedantic -Wall -Wextra -O2 -DNDEBUG ^
    -I"%CORE%" -o "%OUT%\release\startup-gcc.exe" ^
    %SHARED% "%OUT%\release\appboot_res.o" %LIBS%
if errorlevel 1 (echo MinGW release build FAILED & exit /b 1)

set INCLUDE=%VCROOT%\Include;%VCROOT%\MFC\Include;%VCROOT%\ATL\Include
set LIB=%VCROOT%\Lib
set PATH=%VSROOT%\Common\MSDev98\Bin;%VCROOT%\Bin;%PATH%

echo ============ [3/4] MSVC 6.0 cl DEBUG ============
pushd "%OUT%\debug"
rc /r /fo appboot_cl.res "%SRC%appboot.rc"
if errorlevel 1 (echo rc.exe FAILED & popd & exit /b 1)
cl /nologo /TC /W3 /Zi /Od /D WIN32 /D _CONSOLE /D_DEBUG /I"%CORE%" ^
   %SHARED% ^
   /link /MACHINE:X86 appboot_cl.res advapi32.lib user32.lib gdi32.lib kernel32.lib /DEBUG ^
   /OUT:startup-cl.exe
set RC=%ERRORLEVEL%
popd
if not "%RC%"=="0" (echo MSVC debug build FAILED & exit /b 1)

echo ============ [4/4] MSVC 6.0 cl RELEASE ============
pushd "%OUT%\release"
rc /r /fo appboot_cl.res "%SRC%appboot.rc"
if errorlevel 1 (echo rc.exe FAILED & popd & exit /b 1)
cl /nologo /TC /W3 /O2 /D WIN32 /D _CONSOLE /DNDEBUG /I"%CORE%" ^
   %SHARED% ^
   /link /MACHINE:X86 appboot_cl.res advapi32.lib user32.lib gdi32.lib kernel32.lib /OUT:startup-cl.exe
set RC=%ERRORLEVEL%
popd
if not "%RC%"=="0" (echo MSVC release build FAILED & exit /b 1)

echo.
echo OK: debug   = %OUT%\debug\startup-gcc.exe + startup-cl.exe
echo OK: release = %OUT%\release\startup-gcc.exe + startup-cl.exe
endlocal
