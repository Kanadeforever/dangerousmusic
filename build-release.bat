@echo off
chcp 65001 >nul
rem 本脚本负责配置并编译 x64 Release 版本。
rem 任一 CMake 步骤失败都会立即返回非零退出码，避免继续打包旧 DLL。
setlocal
where cmake >nul 2>nul || (echo 未找到 CMake，请先安装 CMake 3.20 或更高版本。& exit /b 1)
cmake -S . -B build -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1
echo.
echo 编译完成：build\Release\dsound.dll
