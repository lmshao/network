# Examples
cmake_minimum_required(VERSION 3.10)

add_subdirectory(tcp)
add_subdirectory(udp)

# Only add unix subdirectory on non-Windows platforms
if(NOT WIN32)
    add_subdirectory(unix)
endif()
