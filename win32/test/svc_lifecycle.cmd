@echo off
rem win32\test\svc_lifecycle.cmd - full service lifecycle test (run elevated)
setlocal
cd /d %~dp0..
rem point the default config (..\conf\startup.conf relative to the exe)
rem at a probe app that records the service actually ran, plus a
rem long-running app used to exercise the control IPC while the
rem service is up (app status / app stop on the default pipe)
echo [svctest]> conf\startup.conf
echo cmd = cmd.exe /c echo via-service ^>^> %CD%\build\svctest.txt>> conf\startup.conf
echo [stayalive]>> conf\startup.conf
echo cmd = cmd.exe /c ping -n 120 127.0.0.1 ^>nul>> conf\startup.conf
echo === install ===
build\release\startup-gcc.exe install
echo rc=%ERRORLEVEL%
echo === start ===
build\release\startup-gcc.exe start
echo rc=%ERRORLEVEL%
timeout /t 3 /nobreak >nul
echo === query ===
sc query AppBootLauncher
echo === control IPC: app status (stayalive must be "running") ===
build\release\startup-gcc.exe app status
echo === control IPC: app stop stayalive ===
build\release\startup-gcc.exe app stop stayalive
echo rc=%ERRORLEVEL%
build\release\startup-gcc.exe app status stayalive
if exist build\svctest.txt (echo PROBE-FILE-OK) else (echo PROBE-FILE-MISSING)
echo === stop ===
build\release\startup-gcc.exe stop
echo rc=%ERRORLEVEL%
echo === uninstall ===
build\release\startup-gcc.exe uninstall
echo rc=%ERRORLEVEL%
del conf\startup.conf
taskkill /f /im ping.exe >nul 2>nul
endlocal
