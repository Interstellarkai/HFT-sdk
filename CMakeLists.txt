# Minimum CMake version chosen for modern target features
cmake_minimum_required(VERSION 3.20)

project(HFT-sdk VERSION 0.1.0 LANGUAGES CXX)

# Enforce modern C++ and keep flags consistent across consumers
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library target
add_library(HFT-sdk
        STATIC
        src/library_anchor.cpp
        src/market/order_book.hpp src/market/order_book.cpp
        src/market/matching_engine.hpp src/market/matching_engine.cpp
        src/market/market_data_publisher.hpp src/market/market_data_publisher.cpp
        src/market/trade_engine.hpp src/market/trade_engine.cpp
        src/market/market_data_engine.hpp src/market/market_data_engine.cpp
        src/benchmark_p99.hpp
        src/ScopeTimer.hpp
        src/HPRingBuffer.hpp
        src/wait_strategies.hpp
        src/common/types.hpp
        src/common/clock.hpp src/common/clock.cpp
        src/orderbook/l3_order_book.cpp src/orderbook/l3_order_book.hpp
        src/orderbook/l2_aggregator.cpp src/orderbook/l2_aggregator.hpp
        src/orderbook/l1_feed.cpp src/orderbook/l1_feed.hpp
        src/common/memory_pool.hpp
        src/common/constants.hpp
        src/risk/risk_engine.cpp src/risk/risk_engine.hpp
        src/latency/latency_model.hpp src/latency/latency_model.cpp
        src/metrics/telemetry.hpp src/metrics/telemetry.cpp
        src/metrics/latency_histogram.hpp src/metrics/latency_histogram.cpp
)


add_library(HFT::sdk ALIAS HFT-sdk)

target_include_directories(HFT-sdk
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/common>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(HFT-sdk PUBLIC cxx_std_23)

# ── Testing ─────────────────────────────────────────────────────────────────
option(HFT_SDK_BUILD_TESTS "Build unit tests" ON)

if (HFT_SDK_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.16.0
    )
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(hft_sdk_tests
            tests/test_hpringbuffer.cpp
            tests/test_l3_order_book.cpp
    )

    target_link_libraries(hft_sdk_tests
            PRIVATE
            HFT-sdk
            GTest::gtest_main
    )

    target_include_directories(hft_sdk_tests
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    include(GoogleTest)
    gtest_discover_tests(hft_sdk_tests)
endif ()
