@echo off
setlocal
call "D:\CX\VS\c\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist build\t mkdir build\t
set DEFS=/D__KERNEL__ /DGU_UEFI_PORT /DGU_AD104_TRANSACTION /DGU_ENTRY_LATCHED_SS1_FULL /D_CRT_SECURE_NO_WARNINGS
set INCS=/Icompat /Ivendor/edk2/Include /Ivendor/edk2/Include/X64 /I.

echo === case: app-flow (self-contained mocks) ===
cl /nologo /std:c11 /O2 /Oi- /GS- /W1 %DEFS% %INCS% /Fo:build\t\ tests\test_app_flow.c /Fe:build\t\test-app-flow.exe > build\t\app.log 2>&1
if errorlevel 1 (echo BUILD FAILED & type build\t\app.log & exit /b 1)
build\t\test-app-flow.exe
echo app-flow-exit=%errorlevel%

echo === case: msi-release (sha256+format+driver fixture) ===
cl /nologo /std:c11 /O2 /Oi- /GS- /W1 %DEFS% %INCS% /Fo:build\t\ tests\test_msi_release.c sha256.c format.c /Fe:build\t\test-msi-release.exe > build\t\msi.log 2>&1
if errorlevel 1 (echo BUILD FAILED & type build\t\msi.log & exit /b 1)
build\t\test-msi-release.exe local\capture
echo msi-release-exit=%errorlevel%
exit /b 0
