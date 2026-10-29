@echo off
setlocal
rem MinGW-w64: set GU_MINGW to your mingw64\bin, or use the WinGet package under your own profile.
if not defined GU_MINGW if exist "%LOCALAPPDATA%\Microsoft\WinGet\Packages" for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*") do if exist "%%~D\mingw64\bin" set "GU_MINGW=%%~D\mingw64\bin"
if defined GU_MINGW set "PATH=%GU_MINGW%;%PATH%"
cd /d "%~dp0"
echo === static invariants (enumeration / exit status / no infinite loop) ===
python verify_port.py
if errorlevel 1 (
  echo INVARIANTS FAILED - do not deploy
  exit /b 1
)
echo === app-flow + msi-release (these were never in this script before) ===
call "%~dp0test-gcc.bat" 2>&1
if errorlevel 1 (
  echo APP-FLOW-OR-MSI-TESTS FAILED - do not deploy
  exit /b 1
)
echo === core build + guards + efi_transaction ===
powershell -NoProfile -ExecutionPolicy Bypass -File Build.ps1 -RunTests -Gcc gcc -Ar ar -Python python -Prom "H:\v\gpu-unlock-4080m\local\capture\ad104-physical-prom.bin" -Assets "H:\v\gpu-unlock-4080m\assets\gpu-unlock\610.43.03" -Config "H:\v\gpu-unlock-4080m\local\capture\config256.txt" 2>&1
echo CORE-EXIT=%errorlevel%
exit /b 0
