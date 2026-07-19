@echo off
chcp 65001 >nul
rem 本脚本在成功编译后生成面向普通用户的最小运行包。
rem 运行包不包含开发者 Hook/RVA 配置；版本号由第一个命令行参数提供。
setlocal EnableExtensions

set "VERSION=%~1"
if "%VERSION%"=="" set "VERSION=dev"

call build-release.bat || exit /b 1

set "NAME=LocalMusic-%VERSION%-win64"
set "OUT=dist\%NAME%"
set "ZIP=dist\%NAME%.zip"

if exist "%OUT%" rmdir /s /q "%OUT%"
if not exist dist mkdir dist
mkdir "%OUT%" || exit /b 1

copy /y "build\Release\dsound.dll" "%OUT%\dsound.dll" >nul || exit /b 1
copy /y "dsound.ini" "%OUT%\dsound.ini" >nul || exit /b 1
copy /y "dsound.zh-hans.ini" "%OUT%\dsound.zh-hans.ini" >nul || exit /b 1
copy /y "dsound.en-us.ini" "%OUT%\dsound.en-us.ini" >nul || exit /b 1
copy /y "安装说明.txt" "%OUT%\安装说明.txt" >nul || exit /b 1
copy /y "README.md" "%OUT%\README.md" >nul || exit /b 1
copy /y "INI配置说明.md" "%OUT%\INI配置说明.md" >nul || exit /b 1
copy /y "CHANGELOG.md" "%OUT%\CHANGELOG.md" >nul || exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%OUT%\*' -DestinationPath '%ZIP%' -Force; $h=(Get-FileHash '%ZIP%' -Algorithm SHA256).Hash.ToLower(); Set-Content -Path '%ZIP%.sha256' -Value ($h+'  %NAME%.zip') -Encoding ascii"
if errorlevel 1 exit /b 1

echo.
echo 发行包：%ZIP%
echo 校验值：%ZIP%.sha256
