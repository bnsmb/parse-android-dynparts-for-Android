cmake_minimum_required(VERSION 3.10)
project(parse-android-dynparts VERSION 0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(GNUInstallDirs)
add_definitions(-D_FILE_OFFSET_BITS=64)

add_subdirectory(liblp)

add_executable(parse-android-dynparts main.cpp)

# Statisches Linken nur für bestimmte Bibliotheken
if(ANDROID)
    # OpenSSL und C++ statisch
    target_link_options(parse-android-dynparts PRIVATE
        -static-libstdc++
        -static-libgcc
        -Wl,-Bstatic -lc++ -lc++abi -l:libcrypto.a -Wl,-Bdynamic
    )
endif()

set_target_properties(parse-android-dynparts PROPERTIES OUTPUT_NAME "mount_dynamic_partitions")
target_compile_options(parse-android-dynparts PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(parse-android-dynparts PRIVATE lp)

install(TARGETS parse-android-dynparts RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

