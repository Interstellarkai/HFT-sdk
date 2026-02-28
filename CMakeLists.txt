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
    src/market/order_book.h src/market/order_book.cpp
    src/market/matching_engine.h src/market/matching_engine.cpp
    src/market/market_data_publisher.h src/market/market_data_publisher.cpp
    src/market/trade_engine.h src/market/trade_engine.cpp
    src/market/market_data_engine.h src/market/market_data_engine.cpp
    src/benchmark_p99.hpp
    src/ScopeTimer.hpp
    src/HPRingBuffer.hpp
    src/common/types.h
    src/common/clock.h src/common/clock.cpp
    src/orderbook/l3_order_book.cpp src/orderbook/l3_order_book.h
    src/orderbook/l2_aggregator.cpp src/orderbook/l2_aggregator.h
    src/orderbook/l1_feed.cpp src/orderbook/l1_feed.h
    src/common/memory_pool.h
    src/common/constants.h
    src/risk/risk_engine.cpp src/risk/risk_engine.h
    src/latency/latency_model.h src/latency/latency_model.cpp
    src/metrics/telemetry.h src/metrics/telemetry.cpp
    src/metrics/latency_histogram.h src/metrics/latency_histogram.cpp
)


add_library(HFT::sdk ALIAS HFT-sdk)

target_include_directories(HFT-sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/common>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(HFT-sdk PUBLIC cxx_std_23)
