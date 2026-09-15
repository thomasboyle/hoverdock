cmake_minimum_required(VERSION 3.25)

if(NOT WIN32)
    message(FATAL_ERROR "Liquid Glass Dock is a native Windows/MSVC project.")
endif()

project(LiquidGlassDock VERSION 1.0.0 LANGUAGES CXX)

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
    src/Renderer.cpp
    src/WindowCatalog.cpp
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
)

target_compile_options(Dock PRIVATE
    /W4
    /WX
    /permissive-
    /EHsc
    /Zc:__cplusplus
    /utf-8
)

target_link_libraries(Dock PRIVATE
    d3d12
    dxgi
    dcomp
    dwmapi
    shcore
    shell32
    ole32
    advapi32
    windowscodecs
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
        DEPENDS "${DOCK_SHADER_SOURCE}"
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
