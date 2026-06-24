cmake_minimum_required(VERSION 3.15)
project(mctde_installer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

add_executable(mctde-installer
    src/main.cpp
    src/Bhd5.cpp
    src/Dcx.cpp
    third_party/miniz.c
)

target_include_directories(mctde-installer PRIVATE src third_party)
