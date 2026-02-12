cmake_minimum_required(VERSION 3.14)
project(agsremotetelemetry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The AGS plugin SDK header (agsplugin.h) must be placed in this directory.
# Download it from:
#   https://github.com/adventuregamestudio/ags/blob/master/Engine/plugin/agsplugin.h

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(agsremotetelemetry SHARED agsremotetelemetry.cpp)

target_include_directories(agsremotetelemetry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agsremotetelemetry PRIVATE CURL::libcurl Threads::Threads)

# AGS plugin naming convention
if(WIN32)
    set_target_properties(agsremotetelemetry PROPERTIES PREFIX "")
    set_target_properties(agsremotetelemetry PROPERTIES OUTPUT_NAME "agsremotetelemetry")
    set_target_properties(agsremotetelemetry PROPERTIES SUFFIX ".dll")
else()
    set_target_properties(agsremotetelemetry PROPERTIES PREFIX "lib")
    set_target_properties(agsremotetelemetry PROPERTIES SUFFIX ".so")
    # Hide all symbols by default, export only what's needed
    target_compile_options(agsremotetelemetry PRIVATE -fvisibility=hidden)
endif()

if(APPLE)
    set_target_properties(agsremotetelemetry PROPERTIES SUFFIX ".dylib")
endif()
