@echo off
REM Build the Memory & VRAM Viewer with MSVC (no CMake needed).
setlocal

set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found. Edit build.bat with your VS path.
    exit /b 1
)
call "%VCVARS%" >nul

set "ROOT=%~dp0"
set "IMGUI=%ROOT%third_party\imgui"
if not exist "%IMGUI%\imgui.cpp" (
    echo Dear ImGui not found - running setup.bat to install dependencies...
    call "%ROOT%setup.bat" || exit /b 1
)

if not exist "%ROOT%build" mkdir "%ROOT%build"
pushd "%ROOT%build"

echo Compiling resources...
rc /nologo /i "%ROOT%." /fo "app.res" "%ROOT%app.rc"
if errorlevel 1 ( echo Resource compile FAILED. & popd & exit /b 1 )

echo Compiling...
cl /nologo /std:c++17 /EHsc /O2 /MT /DNDEBUG /DUNICODE /D_UNICODE ^
   /I"%ROOT%src" /I"%IMGUI%" /I"%IMGUI%\backends" ^
   "%ROOT%src\main.cpp" ^
   "%ROOT%src\metrics.cpp" ^
   "%IMGUI%\imgui.cpp" ^
   "%IMGUI%\imgui_draw.cpp" ^
   "%IMGUI%\imgui_tables.cpp" ^
   "%IMGUI%\imgui_widgets.cpp" ^
   "%IMGUI%\backends\imgui_impl_win32.cpp" ^
   "%IMGUI%\backends\imgui_impl_dx11.cpp" ^
   "app.res" ^
   /link /SUBSYSTEM:WINDOWS /OUT:RVMemViewer.exe

set ERR=%ERRORLEVEL%
popd
if %ERR%==0 (
    echo.
    echo Built: %ROOT%build\RVMemViewer.exe
) else (
    echo.
    echo Build FAILED with code %ERR%.
)
exit /b %ERR%
