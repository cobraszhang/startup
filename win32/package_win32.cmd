@echo off
rem win32\package_win32.cmd - build and refresh the win32 release package:
rem   win32\dist\bin\startup-gcc.exe / startup-cl.exe   (release builds)
rem   win32\dist\conf\startup.conf  - USER configuration: preserved and
rem     never overwritten; seeded from the sample only when missing
rem   win32\dist\README.md          - copy of the project README
setlocal
set ROOT=%~dp0
set DIST=%ROOT%dist

call "%ROOT%build_win32.cmd" || exit /b 1

if not exist "%DIST%\bin"  mkdir "%DIST%\bin"
if not exist "%DIST%\conf" mkdir "%DIST%\conf"
copy /y "%ROOT%build\release\startup-gcc.exe" "%DIST%\bin\" >nul || (echo ERROR: cannot update startup-gcc.exe ^(is the service running it? stop it first^) & exit /b 1)
copy /y "%ROOT%build\release\startup-cl.exe"  "%DIST%\bin\" >nul || (echo ERROR: cannot update startup-cl.exe ^(is the service running it? stop it first^) & exit /b 1)
if not exist "%DIST%\conf\startup.conf" copy /y "%ROOT%conf\startup.conf" "%DIST%\conf\" >nul
copy /y "%ROOT%..\README.md" "%DIST%\README.md" >nul

echo.
echo Package ready: %DIST%
echo   bin\startup-gcc.exe  bin\startup-cl.exe   conf\startup.conf (kept if present)
echo Install: copy win32\dist to any folder, then run
echo   ^<dir^>\bin\startup-gcc.exe install    (auto UAC elevation registers the service)
endlocal
