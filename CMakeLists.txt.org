cmake_minimum_required(VERSION 3.10)

project(parse-android-dynparts VERSION 0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(GNUInstallDirs)

add_definitions(-D_FILE_OFFSET_BITS=64)
add_subdirectory(liblp)

add_executable(parse-android-dynparts main.cpp)
target_compile_options(parse-android-dynparts PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(parse-android-dynparts PRIVATE lp)

install(TARGETS parse-android-dynparts
        RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
