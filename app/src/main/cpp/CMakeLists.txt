cmake_minimum_required(VERSION 3.18)
project(didjdroid CXX)
set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/src)
add_library(didjdroid SHARED
    jni_bridge.cpp
    ${CORE}/arm926.cpp
    ${CORE}/didj.cpp)
target_include_directories(didjdroid PRIVATE ${CORE})
target_compile_options(didjdroid PRIVATE -O3 -fexceptions -Wall -Wno-misleading-indentation)
target_link_libraries(didjdroid log)
