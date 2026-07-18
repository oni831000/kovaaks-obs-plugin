cmake_minimum_required(VERSION 3.28)
project(obs_kovaaks_plugin LANGUAGES CXX)

# Static-link the MSVC runtime so the built DLL doesn't need the redistributable installed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# Dependencies (fetched automatically)
include(FetchContent)

FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz)
FetchContent_MakeAvailable(json)

# OBS Studio SDK paths
# Point these at your local OBS Studio source and build folder.
# Override from the command line, e.g.:
#   cmake -B build -DOBS_ROOT="D:/obs-studio-master"
set(OBS_ROOT "E:/OBS SDK/obs-studio-master" CACHE PATH "Path to the OBS Studio source tree")
set(OBS_BUILD "${OBS_ROOT}/build" CACHE PATH "Path to the OBS Studio build output")

set(OBS_LIB "${OBS_BUILD}/libobs/Release/obs.lib")
set(OBS_FRONTEND_LIB "${OBS_BUILD}/frontend/api/Release/obs-frontend-api.lib")

# Kovaaks Tracker plugin
add_library(kovaaks_plugin MODULE
    src/kovaaks_plugin.cpp
)

target_include_directories(kovaaks_plugin PRIVATE
    ${OBS_ROOT}/libobs
    ${OBS_ROOT}/libobs/util
    ${OBS_ROOT}/frontend/api
    ${OBS_BUILD}/config
)

target_link_libraries(kovaaks_plugin PRIVATE
    ${OBS_LIB}
    ${OBS_FRONTEND_LIB}
    nlohmann_json::nlohmann_json
)

set_target_properties(kovaaks_plugin PROPERTIES PREFIX "")

# Install into a local OBS Studio installation
# Override from the command line, e.g.:
#   cmake -B build -DOBS_INSTALL_DIR="C:/Program Files/obs-studio"
set(OBS_INSTALL_DIR "C:/Program Files/obs-studio" CACHE PATH "Path to your OBS Studio installation")
set(CMAKE_INSTALL_PREFIX "${OBS_INSTALL_DIR}" CACHE PATH "Default install location" FORCE)

install(TARGETS kovaaks_plugin
        RUNTIME DESTINATION "obs-plugins/64bit"
        LIBRARY DESTINATION "obs-plugins/64bit"
)