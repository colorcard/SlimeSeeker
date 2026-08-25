/*
 * SlimeSeeker 性能基准入口。
 *
 * 分别测量位图算子与端到端搜索吞吐，并以逐行 JSON 输出可复现的输入和后端信息；
 * 本程序不是正确性测试，不设置跨机器的绝对性能阈值。
 */
#include "slimeseeker/slimeseeker.h"
#include "backends/backend.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
struct Stats { uint64_t hits = 0; };
int count_results(void *opaque, const ss_result *, size_t count) {
    static_cast<Stats *>(opaque)->hits += count;
    return 0;
}
}

int main(int argc, char **argv) {
    int range = argc > 1 ? std::atoi(argv[1]) : 5000;
    unsigned threads = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 0;
    int threshold = argc > 3 ? std::atoi(argv[3]) : 45;
    if (range <= 0 || threshold < 0 || threshold > SS_DONUT_CELLS) {
        std::fprintf(stderr, "usage: slimeseeker_bench [range] [threads] [threshold]\n");
        return 1;
    }
    for (ss_backend backend : {SS_BACKEND_SCALAR, SS_BACKEND_AUTO, SS_BACKEND_AVX2, SS_BACKEND_NEON}) {
        if (!ss_backend_available(backend)) continue;
        ss_backend selected{};
        auto map_fn = ss::select_backend(backend, selected);
        std::vector<uint8_t> map(512u * 512u);
        std::vector<uint64_t> xterm_scratch(512u);
        constexpr int map_repeats = 20;
        auto map_start = std::chrono::steady_clock::now();
        for (int repeat = 0; repeat < map_repeats; ++repeat)
            map_fn(0, -256 + repeat, -256, 512, 512, map.data(), xterm_scratch.data());
        const double map_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - map_start).count();
        std::printf("{\"phase\":\"map_build\",\"backend_requested\":\"%s\",\"backend_selected\":\"%s\","
                    "\"cells\":%u,\"repeats\":%d,\"elapsed_s\":%.6f,\"cells_per_s\":%.3f}\n",
            ss_backend_name(backend), ss_backend_name(selected), 512u * 512u, map_repeats, map_elapsed,
            static_cast<double>(512u * 512u) * map_repeats / map_elapsed);
        Stats stats;
        ss_search_params_v1 params{sizeof(params), 0, -range, range, -range, range,
                                   static_cast<uint16_t>(threshold), 0};
        ss_search_options_v1 options{sizeof(options), threads, backend, 4096};
        ss_callbacks_v1 callbacks{sizeof(callbacks), &stats, count_results, nullptr, nullptr};
        const auto start = std::chrono::steady_clock::now();
        const auto status = ss_search(&params, &options, &callbacks);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const uint64_t side = static_cast<uint64_t>(range) * 2;
        const uint64_t candidates = side * side;
        std::printf("{\"phase\":\"end_to_end\",\"backend_requested\":\"%s\",\"backend_selected\":\"%s\",\"range\":%d,\"threads\":%u,\"threshold\":%d,\"candidates\":%llu,"
                    "\"hits\":%llu,\"elapsed_s\":%.6f,\"candidates_per_s\":%.3f,\"status\":\"%s\"}\n",
            ss_backend_name(backend), ss_backend_name(selected), range, threads, threshold,
            static_cast<unsigned long long>(candidates),
            static_cast<unsigned long long>(stats.hits), elapsed, static_cast<double>(candidates) / elapsed,
            ss_status_string(status));
        if (status != SS_OK) return status;
    }
    if (ss_backend_available(SS_BACKEND_CUDA)) {
        Stats stats;
        ss_search_params_v1 params{sizeof(params), 0, -range, range, -range, range,
                                   static_cast<uint16_t>(threshold), 0};
        ss_search_options_v1 options{sizeof(options), threads, SS_BACKEND_CUDA, 4096};
        ss_callbacks_v1 callbacks{sizeof(callbacks), &stats, count_results, nullptr, nullptr};
        const auto start = std::chrono::steady_clock::now();
        const auto status = ss_search(&params, &options, &callbacks);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const uint64_t side = static_cast<uint64_t>(range) * 2;
        const uint64_t candidates = side * side;
        std::printf("{\"phase\":\"end_to_end\",\"backend_requested\":\"cuda\",\"backend_selected\":\"cuda\",\"range\":%d,\"threads\":%u,\"threshold\":%d,\"candidates\":%llu,\"hits\":%llu,\"elapsed_s\":%.6f,\"candidates_per_s\":%.3f,\"status\":\"%s\"}\n",
                    range, threads, threshold, static_cast<unsigned long long>(candidates),
                    static_cast<unsigned long long>(stats.hits), elapsed,
                    static_cast<double>(candidates) / elapsed, ss_status_string(status));
        if (status != SS_OK) return status;
    }
    return 0;
}
