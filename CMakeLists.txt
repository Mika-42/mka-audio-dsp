cmake_minimum_required(VERSION 4.3)

project(mka_audio_dsp
        VERSION 0.1.0
        LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(mka_audio_dsp
        src/main.cpp
        src/error.cppm
)

target_sources(mka_audio_dsp
        PRIVATE
        FILE_SET modules
        TYPE CXX_MODULES
        FILES
        src/graph.cppm
 )