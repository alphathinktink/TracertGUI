@echo off
setlocal
cd /d "%~dp0"
if "%TOOLCHAIN%"=="" set TOOLCHAIN=C:\msys64\mingw64\bin
set PATH=%TOOLCHAIN%;%PATH%
echo Using toolchain at "%TOOLCHAIN%"
echo Pass --skip-existing to skip compile commands when matching .o already exists.

echo Building intermediate: "C:\Builder Projects\TracertGUI\WinMainUnit.o"
if /I "%~1"=="--skip-existing" if exist "C:\Builder Projects\TracertGUI\WinMainUnit.o" goto :skip_1
"%TOOLCHAIN%\g++.exe" -c -m64 -O2 -fmax-errors=20 -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare -Wno-deprecated-declarations -Wattributes -Wno-unknown-pragmas -Wno-missing-field-initializers -D_UNICODE -DUNICODE -DNOVTABLE= -D_WIN32_WINNT=0x0600 "WinMainUnit.cpp" -o "C:\Builder Projects\TracertGUI\WinMainUnit.o"
if errorlevel 1 goto :fail
:skip_1
echo Linking target: "C:\Builder Projects\TracertGUI\TracertGUI.exe"
echo Building intermediate: "C:\Builder Projects\TracertGUI\TracertGUI.exe"
if /I "%~1"=="--skip-existing" if exist "C:\Builder Projects\TracertGUI\TracertGUI.exe" goto :skip_2
"%TOOLCHAIN%\g++.exe" -O2 -static -mwindows -m64 -municode "C:\Builder Projects\TracertGUI\WinMainUnit.o" -o "TracertGUI.exe" -lstdc++ -lws2_32 -liphlpapi -lshell32 -luuid -lcomdlg32 -lcomctl32 -lversion -lshlwapi
if errorlevel 1 goto :fail
:skip_2
"%TOOLCHAIN%\g++.exe" -O2 -static -mwindows -m64 -municode "C:\Builder Projects\TracertGUI\WinMainUnit.o" -o "TracertGUI.exe" -lstdc++ -lws2_32 -liphlpapi -lshell32 -luuid -lcomdlg32 -lcomctl32 -lversion -lshlwapi
if errorlevel 1 goto :fail
echo Build finished successfully.
goto :eof
:fail
echo Build failed with error %errorlevel%.
pause
exit /b %errorlevel%
