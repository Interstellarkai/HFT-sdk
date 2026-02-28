{\rtf1\ansi\ansicpg1252\cocoartf2867
\cocoatextscaling0\cocoaplatform0{\fonttbl\f0\fswiss\fcharset0 Helvetica;}
{\colortbl;\red255\green255\blue255;}
{\*\expandedcolortbl;;}
\margl1440\margr1440\vieww36660\viewh19300\viewkind0
\pard\tx720\tx1440\tx2160\tx2880\tx3600\tx4320\tx5040\tx5760\tx6480\tx7200\tx7920\tx8640\pardirnatural\partightenfactor0

\f0\fs24 \cf0 #include "benchmark_p99.hpp"\
#include <array>\
#include <iostream>\
\
int main() \{\
    using namespace hft_bench;\
\
    // Example function we want to benchmark.\
    auto test_fn = [](int x, int y) noexcept \{\
        // Do some very small work\
        return x + y;\
    \};\
\
    constexpr std::size_t MaxSamples = 100'000;\
    std::array<std::int64_t, MaxSamples> latencies\{\};\
\
    std::size_t iterations = 50'000;\
    std::size_t warmups    = 1'000;\
\
    auto stats = benchmark_p99(\
        test_fn,\
        1, 2,                                // args to test_fn\
        std::span<std::int64_t>(latencies),  // buffer\
        iterations,\
        warmups\
    );\
\
    std::cout << "Samples:   " << stats.samples   << "\\n";\
    std::cout << "P99:       " << stats.p99_ns    << " ns\\n";\
    std::cout << "P99.9:     " << stats.p999_ns   << " ns\\n";\
\}}