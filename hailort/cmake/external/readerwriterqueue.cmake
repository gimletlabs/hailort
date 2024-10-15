cmake_minimum_required(VERSION 3.11.0)

if(NOT TARGET readerwriterqueue)
    add_library(readerwriterqueue INTERFACE)
    target_include_directories(readerwriterqueue INTERFACE ${readerwriterqueue_SOURCE_DIR})
endif()
