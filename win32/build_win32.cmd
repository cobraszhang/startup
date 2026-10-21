@echo off
rem win32\build_win32.cmd - build the Win32 launcher with both compilers:
rem   1) MinGW-w64 gcc (d:\my\run\mingw64)
rem   2) Microsoft Visual C++ 6.0 (d:\my\run\Visual C++ 6.0\VC98)
rem Layout: shared core sources in ..\src, win32 sources here, outputs in
rem .\build. All sources are strict C89.
setlocal
set ROOT=%~dp0
set SRC=%ROOT%
set CORE=%ROOT%..\src
set OUT=%ROOT%build
set VCROOT=d:\my\run\Visual C++ 6.0\VC98
set VSROOT=d:\my\run\Visual C++ 6.0
set MINGW=d:\my\run\mingw64\bin

if not exist "%OUT%" mkdir "%OUT%"

echo ============ [1/2] MinGW-w64 gcc ============
"%MINGW%\gcc.exe" -std=c89 -pedantic -Wall -Wextra -O2 ^
    -I"%CORE%" -o "%OUT%\startup-gcc.exe" ^
    "%CORE%\cfg.c" "%CORE%\topo.c" "%CORE%\log.c" "%CORE%\cli.c" ^
    "%SRC%\win32_service.c" "%SRC%\win32_gui.c" ^
    -ladvapi32 -luser32 -lgdi32
if errorlevel 1 (
    echo MinGW build FAILED
    exit /b 1
)

echo ============ [2/2] MSVC 6.0 cl /TC ============
set INCLUDE=%VCROOT%\Include;%VCROOT%\MFC\Include;%VCROOT%\ATL\Include
set LIB=%VCROOT%\Lib
set PATH=%VSROOT%\Common\MSDev98\Bin;%VCROOT%\Bin;%PATH%
pushd "%OUT%"
cl /nologo /TC /W3 /O2 /D WIN32 /D _CONSOLE /I"%CORE%" ^
   "%CORE%\cfg.c" "%CORE%\topo.c" "%CORE%\log.c" "%CORE%\cli.c" ^
   "%SRC%\win32_service.c" "%SRC%\win32_gui.c" ^
   /link advapi32.lib user32.lib gdi32.lib kernel32.lib /OUT:startup-cl.exe
set RC=%ERRORLEVEL%
popd
if not "%RC%"=="0" (
    echo MSVC build FAILED
    exit /b 1
)

echo.
echo OK: %OUT%\startup-gcc.exe  and  %OUT%\startup-cl.exe
endlocal
