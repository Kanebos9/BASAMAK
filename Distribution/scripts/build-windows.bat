@echo off
REM ============================================================================
REM Build BASAMAK from source on Windows.
REM
REM Prerequisites:
REM   - Visual Studio 2022 (Desktop C++ workload) or Build Tools for VS 2022
REM   - CMake 3.22+   (https://cmake.org/download/)
REM   - Git           (https://git-scm.com/download/win)
REM
REM JUCE is cloned to %USERPROFILE%\JUCE if not already present.
REM Override with:  set JUCE_DIR=C:\path\to\JUCE  before running.
REM ============================================================================
setlocal
cd /d "%~dp0..\.."

if "%JUCE_DIR%"=="" if not exist "%USERPROFILE%\JUCE" (
  echo ==^> Cloning JUCE 8.x ...
  git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git "%USERPROFILE%\JUCE"
)

echo ==^> Configuring ^(Release, Visual Studio 2022^) ...
cmake -B build-release -G "Visual Studio 17 2022" -A x64 -DDAVULSEQ_RELEASE=ON
if errorlevel 1 goto :err

echo ==^> Building ...
cmake --build build-release --config Release
if errorlevel 1 goto :err

echo.
echo ==^> Built VST3 is under build-release\DrumSequencer_artefacts\Release\VST3\
echo Run Distribution\scripts\install-windows.bat to install it.
goto :eof

:err
echo Build failed. See the messages above.
exit /b 1
