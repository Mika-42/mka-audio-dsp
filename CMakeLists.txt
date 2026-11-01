cmake_minimum_required(VERSION 4.3)

project(mka-audio-dsp
        VERSION 0.1.0
        DESCRIPTION "DSP graph and execution engine"
        LANGUAGES CXX
)

# ============================================================
# Project options
# ============================================================

option(MKA_BUILD_TESTS
        "Build unit tests"
        ON
)


# ============================================================
# Global C++ configuration
# ============================================================

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
)

FetchContent_MakeAvailable(googletest)


# ============================================================
# DSP library
# ============================================================

add_library(mka-audio-dsp)

target_sources(mka-audio-dsp
        PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS
        src
        FILES
        src/error.cppm
        src/types.cppm
        src/config.cppm
        src/process_context.cppm
        src/node.cppm
        src/channel.cppm
        src/connection.cppm
        src/dag.cppm
        src/execution_plan.cppm
)

target_compile_features(
        mka-audio-dsp
        PUBLIC
        cxx_std_26
)


# ============================================================
# Application
# ============================================================

add_executable(mka_audio_dsp
        src/main.cpp
)

target_link_libraries(mka_audio_dsp
        PRIVATE
        mka-audio-dsp
)

target_compile_features(
        mka_audio_dsp
        PRIVATE
        cxx_std_26
)


# ============================================================
# Tests
# ============================================================

if (MKA_BUILD_TESTS)

    enable_testing()

    add_executable(mka-audio-dsp-tests
            test/test_types.cpp
            test/test_ports.cpp
            test/test_channel_id.cpp
            test/test_audio_channels.cpp
            test/test_config.cpp
            test/test_process_context.cpp
            test/test_node.cpp
            test/test_node_lifetime.cpp
            test/test_backend_io.cpp
            test/test_dag_storage.cpp
            test/test_connection.cpp
            test/test_connect.cpp
            test/test_input_occupancy.cpp
            test/test_disconnect.cpp
            test/test_remove.cpp
            test/test_output_markers.cpp
    )

    target_link_libraries(mka-audio-dsp-tests
            PRIVATE
            mka-audio-dsp
            GTest::gtest_main
    )

    target_compile_features(
            mka-audio-dsp-tests
            PRIVATE
            cxx_std_26
    )

    include(GoogleTest)

    gtest_discover_tests(
            mka-audio-dsp-tests
            TEST_PREFIX "dsp."
    )

    # Keep allocation interception local to this executable: it is not part of
    # the library and does not change the allocator used by application clients.
    add_executable(mka-node-rt-tests test/test_node_rt.cpp)
    target_link_libraries(mka-node-rt-tests PRIVATE mka-audio-dsp GTest::gtest_main)
    target_compile_features(mka-node-rt-tests PRIVATE cxx_std_26)
    gtest_discover_tests(mka-node-rt-tests TEST_PREFIX "dsp.rt.")

endif ()
