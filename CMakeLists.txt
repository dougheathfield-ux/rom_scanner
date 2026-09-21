cmake_minimum_required(VERSION 3.24)
project(rom_scanner VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# --- 1. Find Required System Packages ---
find_package(LibArchive REQUIRED)
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)

# --- 2. Automatically Fetch Dear ImGui ---
include(FetchContent)
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.90.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

# --- 2b. Automatically Download Portable File Dialogs Header ---
set(PFD_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/portable-file-dialogs.h")
if(NOT EXISTS "${PFD_HEADER}")
    message(STATUS "Downloading portable-file-dialogs.h...")
    file(DOWNLOAD
        https://raw.githubusercontent.com/samhocevar/portable-file-dialogs/master/portable-file-dialogs.h
        "${PFD_HEADER}"
        TIMEOUT 15
    )
endif()

# --- 3. Define Executable and Sources ---
add_executable(rom_scanner
    src/main.cpp
    
    # Dear ImGui Core Sources
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp

    # Dear ImGui GLFW + OpenGL3 Backends
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# --- 4. Include Directories ---
target_include_directories(rom_scanner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# --- 5. Link Libraries ---
target_link_libraries(rom_scanner PRIVATE
    LibArchive::LibArchive
    glfw
    OpenGL::GL
)

# Platform-specific thread support for GCC/Clang background worker threads
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(rom_scanner PRIVATE pthread)
endif()
