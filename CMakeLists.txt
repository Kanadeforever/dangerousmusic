# LocalMusic的 CMake 构建入口。
# 本文件只编译一份 64 位 Windows 共享库，并把同一产物复制为 dsound.dll 与 LocalMusic.asi；
# 这样新增文件若未加入构建会在审查时直接可见，不依赖通配符悄悄改变产物。

cmake_minimum_required(VERSION 3.20)
project(LocalMusic LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "本项目只能在 64 位 Windows 环境中编译。")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "请使用 x64 配置进行编译。")
endif()

add_library(dsound SHARED
  src/dllmain.cpp
  src/DSoundProxy.cpp
  src/Logger.cpp
  src/Localization.cpp
  src/Config.cpp
  src/AlbumControls.cpp
  src/DpiAwareness.cpp
  src/Metadata.cpp
  src/NowPlayingOverlay.cpp
  src/FmodApi.cpp
  src/LocalPlayer.cpp
  src/PEImage.cpp
  src/Signature.cpp
  src/InlinePatch.cpp
  src/GameHooks.cpp
  dsound.def
)

target_include_directories(dsound PRIVATE src)
target_compile_features(dsound PRIVATE cxx_std_17)
target_compile_definitions(dsound PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
  target_compile_options(dsound PRIVATE /W4 /EHsc /permissive- /Zc:__cplusplus /utf-8)
  set_property(TARGET dsound PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(dsound PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(dsound PRIVATE user32 shell32 bcrypt ole32 gdi32 gdiplus)
set_target_properties(dsound PROPERTIES PREFIX "" OUTPUT_NAME "dsound")

# ASI 不是第二次编译：构建完成后直接按字节复制同一个 dsound.dll。
# 因此两个文件除文件名/扩展名外完全相同，用户也可以手工直接改名互换。
add_custom_command(TARGET dsound POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          "$<TARGET_FILE:dsound>"
          "$<TARGET_FILE_DIR:dsound>/LocalMusic.asi"
  COMMENT "复制同一二进制为 LocalMusic.asi"
  VERBATIM
)
