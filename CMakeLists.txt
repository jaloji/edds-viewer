cmake_minimum_required(VERSION 3.28)
project(edds-viewer VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "edds-viewer builds Windows components (WIC codec, XnView plugin) and needs Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLLs are loaded into Explorer and XnView, so they must not need a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# All binaries land in one folder next to each other.
foreach(kind RUNTIME LIBRARY ARCHIVE)
    set(CMAKE_${kind}_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} CONFIG)
        set(CMAKE_${kind}_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/bin)
    endforeach()
endforeach()

# DirectXTex (Microsoft, MIT) decodes the DDS pixel formats. Pinned to the may2026 release.
include(FetchContent)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_DX11 OFF CACHE BOOL "" FORCE)
set(BUILD_DX12 OFF CACHE BOOL "" FORCE)
set(BC_USE_OPENMP OFF CACHE BOOL "" FORCE)  # no vcomp140.dll dependency inside Explorer / XnView
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(DirectXTex
    GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
    GIT_TAG 4feb3e11a020f35b796fc769a74216a555d4f5ef  # may2026
    GIT_SHALLOW FALSE
    EXCLUDE_FROM_ALL)  # keeps its headers and .lib out of the release package
FetchContent_MakeAvailable(DirectXTex)

function(edds_warnings target)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    endif()
endfunction()

# EDDS container reader shared by every component.
add_library(edds_core STATIC src/core/edds.cpp src/core/edds.h)
target_include_directories(edds_core PUBLIC src/core)
target_link_libraries(edds_core PUBLIC DirectXTex)
edds_warnings(edds_core)

# Windows Imaging Component codec: Explorer thumbnails, preview pane, photo viewers.
add_library(EddsWic SHARED src/wic/eddswic.cpp src/wic/eddswic.def)
target_link_libraries(EddsWic PRIVATE edds_core windowscodecs ole32 advapi32 shell32)
edds_warnings(EddsWic)

# XnView MP read plugin. XnView only loads files named X*.usr from its Plugins folder.
add_library(Xedds SHARED src/xnview/xedds.cpp src/xnview/xedds.def)
target_link_libraries(Xedds PRIVATE edds_core)
set_target_properties(Xedds PROPERTIES PREFIX "" SUFFIX ".usr")
edds_warnings(Xedds)

# Command-line tools.
add_executable(edds2png tools/edds2png.cpp)
target_link_libraries(edds2png PRIVATE edds_core)
edds_warnings(edds2png)

add_executable(wictest tools/wictest.cpp)
target_link_libraries(wictest PRIVATE windowscodecs ole32)
edds_warnings(wictest)

# Release package (cmake --install build --prefix dist).
install(TARGETS EddsWic Xedds edds2png wictest RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES scripts/install.bat scripts/uninstall.bat scripts/install-xnview.bat
              scripts/uninstall-xnview.bat scripts/clear-thumbnails.bat README.md LICENSE
        DESTINATION .)
# DirectXTex is linked statically: its MIT notice ships with the binaries.
install(FILES ${directxtex_SOURCE_DIR}/LICENSE DESTINATION . RENAME DirectXTex-LICENSE.txt)
