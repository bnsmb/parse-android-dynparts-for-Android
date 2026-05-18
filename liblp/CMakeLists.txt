# liblp/CMakeLists.txt
add_library(lp STATIC
    reader.cpp
    utility.cpp
    # ... weitere Quellcodedateien
)

target_include_directories(lp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# KEIN target_link_libraries(lp ...) für OpenSSL mehr
# Die Symbole werden später vom Hauptexecutable bereitgestellt
