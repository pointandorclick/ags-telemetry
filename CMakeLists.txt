cmake_minimum_required(VERSION 3.16)
project(agstelemetry CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_TEST_DRIVER "Build the standalone CLI test driver" OFF)

set(CLIENT_SOURCES
    src/telemetry_client.cpp
    src/json_util.cpp
)

if(WIN32)
    list(APPEND CLIENT_SOURCES src/http_winhttp.cpp)
else()
    list(APPEND CLIENT_SOURCES src/http_curl.cpp)
endif()

add_library(agstelemetry SHARED src/plugin.cpp ${CLIENT_SOURCES})
target_include_directories(agstelemetry PRIVATE include)

if(WIN32)
    # AGS editor/engine look for ags*.dll (no lib prefix)
    set_target_properties(agstelemetry PROPERTIES PREFIX "")
    target_link_libraries(agstelemetry PRIVATE winhttp)
    if(MINGW)
        # Self-contained DLL: no libgcc/libstdc++/winpthread DLL dependencies
        target_link_options(agstelemetry PRIVATE
            -static-libgcc -static-libstdc++
            -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive -Wl,-Bdynamic)
    endif()
elseif(APPLE)
    # AGS POSIX loader resolves plugin "agstelemetry" as libagstelemetry.dylib
    find_library(CURL_LIB curl REQUIRED)
    target_link_libraries(agstelemetry PRIVATE ${CURL_LIB})
else()
    find_package(CURL REQUIRED)
    target_link_libraries(agstelemetry PRIVATE CURL::libcurl pthread)
endif()

if(BUILD_TEST_DRIVER)
    add_executable(telemetry-driver test/driver.cpp ${CLIENT_SOURCES})
    target_include_directories(telemetry-driver PRIVATE include src)
    if(WIN32)
        target_link_libraries(telemetry-driver PRIVATE winhttp)
    elseif(APPLE)
        target_link_libraries(telemetry-driver PRIVATE ${CURL_LIB})
    else()
        target_link_libraries(telemetry-driver PRIVATE CURL::libcurl pthread)
    endif()
endif()
