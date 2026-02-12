cmake_minimum_required(VERSION 3.14)
project(agsremotetelemetry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The AGS plugin SDK header (agsplugin.h) must be placed in this directory.
# Download it from:
#   https://github.com/adventuregamestudio/ags/blob/master/Engine/plugin/agsplugin.h

find_package(Threads REQUIRED)

add_library(agsremotetelemetry SHARED agsremotetelemetry.cpp)
target_include_directories(agsremotetelemetry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    # Windows: use WinHTTP (built-in, no external dependencies)
    target_link_libraries(agsremotetelemetry PRIVATE winhttp Threads::Threads)
    set_target_properties(agsremotetelemetry PROPERTIES PREFIX "")
    set_target_properties(agsremotetelemetry PROPERTIES OUTPUT_NAME "agsremotetelemetry")
    set_target_properties(agsremotetelemetry PROPERTIES SUFFIX ".dll")
else()
    # macOS / Linux: use libcurl
    find_package(CURL REQUIRED)
    target_link_libraries(agsremotetelemetry PRIVATE CURL::libcurl Threads::Threads)
    set_target_properties(agsremotetelemetry PROPERTIES PREFIX "lib")
    target_compile_options(agsremotetelemetry PRIVATE -fvisibility=hidden)
    if(APPLE)
        set_target_properties(agsremotetelemetry PROPERTIES SUFFIX ".dylib")
    else()
        set_target_properties(agsremotetelemetry PROPERTIES SUFFIX ".so")
    endif()
endif()
