@echo off
rem win32\test\svc_lifecycle.cmd - full service lifecycle test (run elevated;
rem when not elevated the install/uninstall steps exercise the auto-UAC path).
rem Verifies: install -> start -> probe file written -> control IPC over the
rem default pipe -> stop -> uninstall -> service truly GONE (sc query 1060).
setlocal
cd /d %~dp0..
set EXE=build\release\startup-gcc.exe

rem leave no stale registration behind (idempotent reruns)
%EXE% uninstall >nul 2>nul
sc query AppBootLauncher | find "1060" >nul || (echo PRECLEAN-FAIL & exit /b 1)

rem the service reads ..\conf\startup.conf relative to the exe: point it at
rem a probe app plus a long-running app used for the control IPC checks.
rem the sample config is backed up and restored around the run.
if exist conf\startup.conf move /y conf\startup.conf conf\startup.conf.bak >nul
echo [svctest]> conf\startup.conf
echo cmd = cmd.exe /c echo via-service ^>^> %CD%\build\svctest.txt>> conf\startup.conf
echo [stayalive]>> conf\startup.conf
echo cmd = cmd.exe /c ping -n 120 127.0.0.1 ^>nul>> conf\startup.conf

echo === install ===
%EXE% install
echo rc=%ERRORLEVEL%
echo === start ===
%EXE% start
echo rc=%ERRORLEVEL%
timeout /t 3 /nobreak >nul
echo === query ===
sc query AppBootLauncher
echo === control IPC: app status (stayalive must be "running") ===
%EXE% app status
echo === control IPC: app stop stayalive ===
%EXE% app stop stayalive
echo rc=%ERRORLEVEL%
%EXE% app status stayalive
if exist build\svctest.txt (echo PROBE-FILE-OK) else (echo PROBE-FILE-MISSING)
echo === stop ===
%EXE% stop
echo rc=%ERRORLEVEL%
echo === uninstall ===
%EXE% uninstall
echo rc=%ERRORLEVEL%
echo === verify service is really gone (sc query must report 1060) ===
sc query AppBootLauncher | find "1060" >nul && (echo GONE-OK) || (echo STILL-PRESENT-FAIL)

if exist conf\startup.conf del /q conf\startup.conf
if exist conf\startup.conf.bak move /y conf\startup.conf.bak conf\startup.conf >nul
taskkill /f /im ping.exe >nul 2>nul
endlocal
