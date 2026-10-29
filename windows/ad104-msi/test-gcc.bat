@echo off
setlocal
rem MinGW-w64: set GU_MINGW to your mingw64\bin, or use the WinGet package under your own profile.
if not defined GU_MINGW if exist "%LOCALAPPDATA%\Microsoft\WinGet\Packages" for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*") do if exist "%%~D\mingw64\bin" set "GU_MINGW=%%~D\mingw64\bin"
if defined GU_MINGW set "PATH=%GU_MINGW%;%PATH%"
cd /d "%~dp0"
if not exist build\t mkdir build\t
set FLAGS=-std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-misleading-indentation -D__KERNEL__ -DGU_UEFI_PORT -DGU_AD104_TRANSACTION -DGU_ENTRY_LATCHED_SS1_FULL -Icompat -Ivendor/edk2/Include -Ivendor/edk2/Include/X64

echo === app-flow (GCC) ===
gcc %FLAGS% tests\test_app_flow.c -o build\t\app-gcc.exe
if errorlevel 1 (echo GCC-COMPILE-FAILED & exit /b 1)
build\t\app-gcc.exe
echo app-exit=%errorlevel%

echo === msi-release (GCC, fixture=%1) ===
gcc %FLAGS% tests\test_msi_release.c sha256.c format.c -o build\t\msi-gcc.exe
if errorlevel 1 (echo GCC-COMPILE-FAILED & exit /b 1)
build\t\msi-gcc.exe local\capture
echo msi-exit=%errorlevel%
exit /b 0
