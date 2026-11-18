cmake_minimum_required(VERSION 3.25)
project(BorderlessMouse CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "只支持 MSVC 构建")
endif()

# 静态 CRT：发布物是单个 exe，不依赖 VC 运行库；ASAN 需要动态 CRT，仅用于定位内存问题
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(BM_ASAN "用 AddressSanitizer 构建" OFF)
if(BM_ASAN)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

file(GLOB_RECURSE BM_SOURCES CONFIGURE_DEPENDS src/core/*.cpp src/app/*.cpp)

set(BM_RESOURCES src/app/resources/app.rc)

add_executable(BorderlessMouse WIN32 ${BM_SOURCES} ${BM_RESOURCES})

target_include_directories(BorderlessMouse PRIVATE src)
target_compile_definitions(BorderlessMouse PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
target_compile_options(BorderlessMouse PRIVATE /utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus)
target_link_libraries(BorderlessMouse PRIVATE
  user32 gdi32 gdiplus dwmapi shell32 shlwapi ole32 oleaut32 comctl32
  ws2_32 bcrypt crypt32 windowscodecs comdlg32 advapi32 wtsapi32)

# 清单由 app.rc 内嵌，禁止链接器再生成一份，否则资源重复
target_link_options(BorderlessMouse PRIVATE /OPT:REF /OPT:ICF /MANIFEST:NO)

if(BM_ASAN)
  target_compile_options(BorderlessMouse PRIVATE /fsanitize=address /Od /Zi)
  target_link_options(BorderlessMouse PRIVATE /INCREMENTAL:NO)
endif()