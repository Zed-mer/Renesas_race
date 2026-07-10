@echo off
setlocal
set "PYOCD=C:\100ASK_PYOCD\pyocd311\Scripts\pyocd.exe"
set "PYOCD_CONFIG=C:\100ASK_PYOCD\pyocd.yaml"
set "PYOCD_SCRIPT=%~dp0pyocd_user.py"

if not exist "%PYOCD%" (
  echo pyocd.exe not found: %PYOCD%
  exit /b 1
)

if not exist "%PYOCD_CONFIG%" (
  echo pyocd config not found: %PYOCD_CONFIG%
  exit /b 1
)

"%PYOCD%" gdbserver --config "%PYOCD_CONFIG%" --script "%PYOCD_SCRIPT%" --port 3333 --telnet-port 4444 --probe-server-port 5555 --persist --core 0
endlocal
