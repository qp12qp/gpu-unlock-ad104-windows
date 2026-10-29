@echo off
setlocal
rem MinGW-w64: set GU_MINGW to your mingw64\bin, or use the WinGet package under your own profile.
if not defined GU_MINGW if exist "%LOCALAPPDATA%\Microsoft\WinGet\Packages" for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*") do if exist "%%~D\mingw64\bin" set "GU_MINGW=%%~D\mingw64\bin"
if defined GU_MINGW set "PATH=%GU_MINGW%;%PATH%"
call "D:\CX\VS\c\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
copy /y build\AD104-EXPERIMENTAL.EFI build\AD104-EXPERIMENTAL.msvc.efi >nul 2>&1
where gcc
gcc --version | findstr /C:"gcc"
powershell -NoProfile -ExecutionPolicy Bypass -File Build-Experimental.ps1 -Gcc gcc -Python python -VcVars64 "D:\CX\VS\c\VC\Auxiliary\Build\vcvars64.bat" 2>&1
echo SCRIPT-EXIT=%errorlevel%
if exist build\AD104-EXPERIMENTAL.EFI (dir /b build\AD104-EXPERIMENTAL.EFI & certutil -hashfile build\AD104-EXPERIMENTAL.EFI SHA256 | findstr /V ":")
exit /b 0
