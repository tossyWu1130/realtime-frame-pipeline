@echo off
REM Run the demo with the Qt runtime on PATH.
REM Requires Qt (6.9.2 tested) and the MinGW that ships with it.

set "QT_BIN=D:\qt\6.9.2\mingw_64\bin"
set "MINGW_BIN=D:\qt\Tools\mingw1310_64\bin"
set "PATH=%QT_BIN%;%MINGW_BIN%;%PATH%"

cd /d "%~dp0"

if exist "build\realtime_frame_pipeline.exe" (
    "build\realtime_frame_pipeline.exe" %*
    goto :end
)

if exist "out\realtime_frame_pipeline.exe" (
    "out\realtime_frame_pipeline.exe" %*
    goto :end
)

echo Build not found. Configure and build first, for example:
echo   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/qt/6.9.2/mingw_64
echo   cmake --build build

:end
pause
