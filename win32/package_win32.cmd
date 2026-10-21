@echo off
rem win32\package_win32.cmd - build then assemble the win32 release package:
rem   win32\dist\bin\startup-gcc.exe    (MinGW-w64 build)
rem   win32\dist\bin\startup-cl.exe     (MSVC 6.0 build)
rem   win32\dist\conf\startup.conf      (default config: bin\.. -> conf)
rem   win32\dist\README.md              (copy of the project README)
setlocal
set ROOT=%~dp0
set DIST=%ROOT%dist

call "%ROOT%build_win32.cmd" || exit /b 1

if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%\bin"
mkdir "%DIST%\conf"
copy /y "%ROOT%build\release\startup-gcc.exe" "%DIST%\bin\" >nul
copy /y "%ROOT%build\release\startup-cl.exe"  "%DIST%\bin\" >nul
copy /y "%ROOT%conf\startup.conf"     "%DIST%\conf\" >nul
copy /y "%ROOT%..\README.md"          "%DIST%\README.md" >nul

echo.
echo Package ready: %DIST%
echo   bin\startup-gcc.exe  bin\startup-cl.exe   conf\startup.conf
echo Install: copy win32\dist to any folder, then run
echo   ^<dir^>\bin\startup-gcc.exe install    (needs admin, registers auto-start service)
endlocal
