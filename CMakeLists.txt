cmake_minimum_required(VERSION 3.25)

if(NOT WIN32)
    message(FATAL_ERROR "Liquid Glass Dock is a native Windows/MSVC project.")
endif()

project(LiquidGlassDock VERSION 1.1.9 LANGUAGES CXX)

# Tag builds override the version: -DDOCK_VERSION=1.2.3 (release.yml passes the
# git tag through). Defaults to the project version above.
if(DEFINED DOCK_VERSION AND NOT DOCK_VERSION STREQUAL "")
  set(DOCK_EFFECTIVE_VERSION "${DOCK_VERSION}")
else()
  set(DOCK_EFFECTIVE_VERSION "${PROJECT_VERSION}")
endif()

# Update feed identity. Override with -DDOCK_REPO_OWNER=... -DDOCK_REPO_NAME=...
# for forks so auto-updates follow the fork's releases.
set(DOCK_REPO_OWNER "thomasboyle" CACHE STRING "GitHub repo owner for auto-updates")
set(DOCK_REPO_NAME "hoverdock" CACHE STRING "GitHub repo name for auto-updates")

if(NOT MSVC)
    message(FATAL_ERROR "Liquid Glass Dock is a native Windows/MSVC project.")
endif()

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("/std:c++26" DOCK_HAS_CXX26)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_GENERATOR_PLATFORM x64 CACHE STRING "Build x64 only" FORCE)

add_executable(Dock WIN32
    src/main.cpp
    src/DockApp.cpp
    src/DockConfig.cpp
    src/InstalledApps.cpp
    src/Renderer.cpp
    src/Startup.cpp
    src/SystemTray.cpp
    src/TypeSafeClient.cpp
    src/Updater.cpp
    src/WindowCatalog.cpp
    src/PerfBoost.cpp
)

target_include_directories(Dock PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(DOCK_HAS_CXX26)
    target_compile_options(Dock PRIVATE /std:c++26)
else()
    target_compile_options(Dock PRIVATE /std:c++latest)
endif()

target_compile_definitions(Dock PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    DOCK_VERSION_STRING="${DOCK_EFFECTIVE_VERSION}"
    DOCK_REPO_OWNER="${DOCK_REPO_OWNER}"
    DOCK_REPO_NAME="${DOCK_REPO_NAME}"
)

target_compile_options(Dock PRIVATE
    /W4
    /WX
    /permissive-
    /EHsc
    /Zc:__cplusplus
    /utf-8
    $<$<CONFIG:Release>:/O2 /Oi /Gy /Zi>
)

# Release PDBs: no codegen change, but crash dumps become analyzable.
target_link_options(Dock PRIVATE
    $<$<CONFIG:Release>:/DEBUG>
)

set_property(TARGET Dock PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

target_link_libraries(Dock PRIVATE
    d3d12
    dxgi
    dcomp
    dwmapi
    shcore
    shell32
    ole32
    oleaut32
    taskschd
    advapi32
    propsys
    windowscodecs
    gdi32
    version
    wtsapi32
    psapi
    winhttp
    comctl32
    wlanapi
    powrprof
    dxva2
)

set(DOCK_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/Shaders.hlsl")
set(DOCK_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

find_program(DXC_EXECUTABLE
    NAMES dxc.exe
    HINTS
        "$ENV{WindowsSdkDir}bin/x64"
        "$ENV{WindowsSdkDir}bin/$ENV{WindowsSDKVersion}x64"
)

if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR
        "dxc.exe from the installed Windows SDK was not found. "
        "Set -DDXC_EXECUTABLE=\"C:/Program Files (x86)/Windows Kits/10/bin/<SDK>/x64/dxc.exe\".")
endif()

function(add_dxc_header output variable target entrypoint)
    add_custom_command(
        OUTPUT "${DOCK_SHADER_OUTPUT_DIR}/${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DOCK_SHADER_OUTPUT_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -nologo -E "${entrypoint}" -T "${target}"
                -O3 -Ges -HV 2021 -Vn "${variable}"
                -Fh "${DOCK_SHADER_OUTPUT_DIR}/${output}" "${DOCK_SHADER_SOURCE}"
        DEPENDS "${DOCK_SHADER_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/src/DockTheme.hlsli"
        VERBATIM
        COMMENT "Compiling ${target} ${entrypoint}"
    )
    list(APPEND DOCK_SHADER_HEADERS "${DOCK_SHADER_OUTPUT_DIR}/${output}")
    set(DOCK_SHADER_HEADERS "${DOCK_SHADER_HEADERS}" PARENT_SCOPE)
endfunction()

add_dxc_header(glass_vs_60.h gGlassVs60 vs_6_0 FullscreenVS)
add_dxc_header(glass_ps_60.h gGlassPs60 ps_6_0 GlassPS)
add_dxc_header(icon_vs_60.h gIconVs60 vs_6_0 IconVS)
add_dxc_header(icon_ps_60.h gIconPs60 ps_6_0 IconPS)
add_dxc_header(glass_vs_66.h gGlassVs66 vs_6_6 FullscreenVS)
add_dxc_header(glass_ps_66.h gGlassPs66 ps_6_6 GlassPS)
add_dxc_header(icon_vs_66.h gIconVs66 vs_6_6 IconVS)
add_dxc_header(icon_ps_66.h gIconPs66 ps_6_6 IconPS)

add_custom_target(DockShaders DEPENDS ${DOCK_SHADER_HEADERS})
add_dependencies(Dock DockShaders)

set_property(TARGET Dock PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:Dock>")

# --- NSIS installer -------------------------------------------------------
# Per-user install (no elevation) so silent auto-updates work without UAC:
#   cmake --build out --config Release --target installer
# Requires makensis on PATH (winget install NSIS.NSIS, or the CI job).
find_program(MAKENSIS_EXECUTABLE NAMES makensis makensis.exe)
if(MAKENSIS_EXECUTABLE)
    set(DOCK_INSTALLER_OUTPUT "${CMAKE_BINARY_DIR}/Hoverdock-Setup-${DOCK_EFFECTIVE_VERSION}.exe")
    # NSIS File does not match forward-slash paths, but CMake paths always use
    # them: stage the exe via cmake -E (which accepts either form) and hand
    # makensis native backslash paths only.
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/staging/Dock.exe" DOCK_STAGED_EXE_NATIVE)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" DOCK_SRC_DIR_NATIVE)
    file(TO_NATIVE_PATH "${DOCK_INSTALLER_OUTPUT}" DOCK_INSTALLER_OUTPUT_NATIVE)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/installer/hoverdock.nsi" DOCK_NSI_NATIVE)
    add_custom_target(installer
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/staging"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:Dock>"
            "${CMAKE_BINARY_DIR}/staging/Dock.exe"
        COMMAND "${MAKENSIS_EXECUTABLE}"
            /DVERSION=${DOCK_EFFECTIVE_VERSION}
            "/DSRCBIN=${DOCK_STAGED_EXE_NATIVE}"
            "/DSRCDIR=${DOCK_SRC_DIR_NATIVE}"
            "/DOUTFILE=${DOCK_INSTALLER_OUTPUT_NATIVE}"
            "${DOCK_NSI_NATIVE}"
        DEPENDS Dock
        COMMENT "Building NSIS installer ${DOCK_INSTALLER_OUTPUT}"
        VERBATIM
    )
else()
    add_custom_target(installer
        COMMAND "${CMAKE_COMMAND}" -E echo "makensis not found on PATH; install NSIS to build the installer."
        COMMAND_EXPAND_LISTS
    )
endif()
