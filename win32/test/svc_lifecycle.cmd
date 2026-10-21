@echo off
rem win32\test\svc_lifecycle.cmd - full service lifecycle test (run elevated)
setlocal
cd /d %~dp0..
rem point the default config (..\conf\startup.conf relative to the exe)
rem at a probe app that records the service actually ran
echo [svctest]> conf\startup.conf
echo cmd = cmd.exe /c echo via-service ^>^> %CD%\build\svctest.txt>> conf\startup.conf
echo === install ===
build\release\startup-gcc.exe install
echo rc=%ERRORLEVEL%
echo === start ===
build\release\startup-gcc.exe start
echo rc=%ERRORLEVEL%
timeout /t 3 /nobreak >nul
echo === query ===
sc query AppBootLauncher
echo === stop ===
build\release\startup-gcc.exe stop
echo rc=%ERRORLEVEL%
echo === uninstall ===
build\release\startup-gcc.exe uninstall
echo rc=%ERRORLEVEL%
del conf\startup.conf
endlocal
