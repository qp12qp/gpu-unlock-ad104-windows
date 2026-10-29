@echo off
setlocal
rem MinGW-w64: set GU_MINGW to your mingw64\bin, or use the WinGet package under your own profile.
if not defined GU_MINGW if exist "%LOCALAPPDATA%\Microsoft\WinGet\Packages" for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*") do if exist "%%~D\mingw64\bin" set "GU_MINGW=%%~D\mingw64\bin"
if defined GU_MINGW set "PATH=%GU_MINGW%;%PATH%"
cd /d "%~dp0"
if not exist build\t mkdir build\t
set FLAGS=-std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-misleading-indentation -D__KERNEL__ -DGU_UEFI_PORT -DGU_AD104_TRANSACTION -DGU_ENTRY_LATCHED_SS1_FULL -Icompat -Ivendor/edk2/Include -Ivendor/edk2/Include/X64
gcc %FLAGS% tests\test_boot_io.c sha256.c format.c -o build\t\boot-io.exe
if errorlevel 1 (echo COMPILE-FAILED & exit /b 1)
build\t\boot-io.exe "H:\v\gpu-unlock-4080m\bootmgfw.efi" "H:\v\gpu-unlock-4080m\assets\gpu-unlock\610.43.03"
echo bootio-exit=%errorlevel%
exit /b 0
