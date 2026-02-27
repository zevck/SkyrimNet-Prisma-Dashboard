cmake_minimum_required(VERSION 3.21)

# Force static linking for all vcpkg dependencies
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "" FORCE)

project(
    SkyrimNetPrismaDashboard
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Output directly to mod folder
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Install prefix" FORCE)

# ---- CommonLibSSE-NG ----
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/CommonLibSSE-NG EXCLUDE_FROM_ALL)

# ---- Plugin Target ----
add_library(
    ${PROJECT_NAME}
    SHARED
    src/main.cpp
    src/keyhandler/keyhandler.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_precompile_headers(
    ${PROJECT_NAME}
    PRIVATE
        src/pch.h
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        CommonLibSSE::CommonLibSSE
)

# ---- Installation ----
install(
    FILES $<TARGET_FILE:${PROJECT_NAME}>
    DESTINATION "SKSE/Plugins"
)

install(
    FILES $<TARGET_PDB_FILE:${PROJECT_NAME}>
    DESTINATION "SKSE/Plugins"
    OPTIONAL
)
