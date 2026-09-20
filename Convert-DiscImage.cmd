@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" goto usage
if not "%~2"=="" goto usage
if not exist "%~dp0Convert-DiscImage.ps1" goto missingScript

echo Converting disc image. The original file will not be changed.
echo.
set "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Convert-DiscImage.ps1" -SourcePath "%~f1"
set "conversionExit=%errorlevel%"
echo.
if "%conversionExit%"=="0" (
    echo Conversion complete. See Path above for the usable ISO.
) else (
    echo Conversion failed. See the error above.
)
goto finish

:usage
echo Drag one ISO or BIN image onto Convert-DiscImage.cmd.
echo Keep this file next to Convert-DiscImage.ps1.
set "conversionExit=2"
goto finish

:missingScript
echo Convert-DiscImage.ps1 was not found next to this file.
set "conversionExit=1"

:finish
pause
exit /b %conversionExit%