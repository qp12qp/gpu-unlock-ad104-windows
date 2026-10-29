@echo off
setlocal enabledelayedexpansion
call "D:\CX\VS\c\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
del /q build\*.obj 2>nul

set DEFS=/D__KERNEL__ /DGU_UEFI_PORT /DGU_AD104_TRANSACTION /DGU_ENTRY_LATCHED_SS1_FULL /D_CRT_SECURE_NO_WARNINGS
set INCS=/Icompat /Ivendor/edk2/Include /Ivendor/edk2/Include/X64
set CFLAGS=/nologo /c /std:c11 /O2 /GS- /W3 /Fo:build\

for %%S in (port compat checks profile sha256 runtime format app boot_io msi_release vendor\unlock_610 vendor\prep_610 vendor\layout_610 vendor\vbios_610 vendor\gfw_eval) do (
  set EXTRA=
  if /I "%%~nS"=="runtime" set EXTRA=/Oi-
  cl %CFLAGS% !EXTRA! %DEFS% %INCS% %%S.c > build\%%~nS.log 2>&1
  if errorlevel 1 (
    echo === COMPILE FAILED: %%S.c ===
    type build\%%~nS.log
    exit /b 1
  )
  findstr /C:"warning" build\%%~nS.log >nul && (echo WARN in %%S: & findstr /C:"warning" build\%%~nS.log)
)
echo COMPILE-ALL-OK

link /nologo /subsystem:efi_application /entry:UefiMain /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /brepro /out:build\AD104-EXPERIMENTAL.EFI build\*.obj > build\link.log 2>&1
if errorlevel 1 (echo === LINK FAILED === & type build\link.log & exit /b 1)
echo LINK-OK
dir /b build\AD104-EXPERIMENTAL.EFI
exit /b 0
