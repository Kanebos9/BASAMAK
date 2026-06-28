@echo off
REM ============================================================================
REM BASAMAK - Windows installer.  Double-click this file to run.
REM
REM Installs the VST3 for all hosts and seeds the factory sample library.
REM Your samples/sounds/presets live in Documents\BASAMAK and are never
REM touched, so this also works as an in-place update.
REM ============================================================================
setlocal
set "HERE=%~dp0"
set "VST3_DST=%CommonProgramFiles%\VST3"

REM Resolve the REAL Documents folder the SAME way the plugin does. With OneDrive
REM "Known Folder" backup enabled, Documents is redirected (e.g. ...\OneDrive\Documents),
REM and a plain %USERPROFILE%\Documents points at the WRONG, empty folder - so the
REM seeded samples would never appear in the plugin. GetFolderPath('MyDocuments')
REM honors the redirection exactly like JUCE's userDocumentsDirectory in the plugin.
for /f "usebackq delims=" %%D in (`powershell -NoProfile -Command "[Environment]::GetFolderPath('MyDocuments')"`) do set "DOCS=%%D"
if not defined DOCS set "DOCS=%USERPROFILE%\Documents"
set "DATA=%DOCS%\BASAMAK"
set "SAMPLES_DST=%DATA%\Samples"

echo ==^> Installing BASAMAK ...

REM Installing to the shared VST3 folder needs admin rights. Re-launch elevated
REM if we can't write there.
mkdir "%VST3_DST%" 2>nul
echo test> "%VST3_DST%\.basamak_write_test" 2>nul
if not exist "%VST3_DST%\.basamak_write_test" (
  echo Requesting administrator rights to write to %VST3_DST% ...
  powershell -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
  exit /b
)
del "%VST3_DST%\.basamak_write_test" 2>nul

mkdir "%SAMPLES_DST%"        2>nul
mkdir "%DATA%\Sound Bank"    2>nul
mkdir "%DATA%\Presets"       2>nul

REM Replace any previous plugin binary (user data is elsewhere, left intact).
if exist "%VST3_DST%\BASAMAK.vst3" rmdir /s /q "%VST3_DST%\BASAMAK.vst3"
xcopy /e /i /y "%HERE%BASAMAK.vst3" "%VST3_DST%\BASAMAK.vst3" >nul

REM Seed factory samples - /d copies only files that are newer/missing, so your
REM own additions are preserved.
if exist "%HERE%Samples" (
  echo ==^> Adding factory samples ^(existing files are kept^) ...
  xcopy /e /i /d /y "%HERE%Samples" "%SAMPLES_DST%" >nul
)

echo.
echo ==^> Done. BASAMAK installed to:
echo     %VST3_DST%\BASAMAK.vst3
echo     Library: %DATA%
echo.
echo Restart your DAW and rescan plugins.
pause
