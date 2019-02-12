cmake_minimum_required(VERSION 3.8)
project(CompileTimer)


# ===============================================
# Global settings

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# ===============================================
# Create Binary

file(GLOB_RECURSE SOURCES
    "src/*.cc"
    "src/*.hh"
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})

add_executable(CompileTimer ${SOURCES})

target_include_directories(CompileTimer PUBLIC "src")

if (MSVC)
    target_compile_options(CompileTimer PUBLIC
        /MP
    )
else()
    target_compile_options(CompileTimer PUBLIC
        -Wall
        -Wno-unused-variable
    )
endif()
