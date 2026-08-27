/*
 * SlimeSeeker 传统 CLI 实现。
 *
 * 只负责参数解析、信号处理、结果保留策略与终端输出；搜索算法全部通过公共 C ABI 调用。
 */
#include "cli.hpp"
#include "biome_score.hpp"
#include "slimeseeker/slimeseeker.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace ss::cli {
namespace {
std::atomic<bool> interrupted{false};

struct Better {
    bool operator()(const ss_result &a, const ss_result &b) const {
        if (a.count != b.count) return a.count > b.count;
        const int64_t da = static_cast<int64_t>(a.x) * a.x + static_cast<int64_t>(a.z) * a.z;
        const int64_t db = static_cast<int64_t>(b.x) * b.x + static_cast<int64_t>(b.z) * b.z;
        if (da != db) return da < db;
        if (a.x != b.x) return a.x < b.x;
        return a.z < b.z;
    }
};

enum class ResultMode { all, stream, top };
struct Context {
    ResultMode mode = ResultMode::all;
    bool csv = false;
    bool progress = true;
    bool benchmark = false;
    uint64_t top_count = 0;
    std::vector<ss_result> all;
    std::priority_queue<ss_result, std::vector<ss_result>, Better> top;
    std::unique_ptr<BiomeScorer> biome_scorer;
    std::string callback_error;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

void print_result(const ss_result &result, bool csv) {
    if (csv) std::printf("%d,%d,%u\n", result.x, result.z, result.count);
    else std::printf("(%d, %d)   %u\n", result.x, result.z, result.count);
}

int receive_results(void *opaque, const ss_result *results, size_t count) {
    auto &ctx = *static_cast<Context *>(opaque);
    if (ctx.benchmark) return 0;
    if (ctx.biome_scorer) {
        try {
            for (size_t i = 0; i < count; ++i) ctx.biome_scorer->consider(results[i]);
        } catch (const std::exception &error) {
            ctx.callback_error = error.what();
            return 1;
        }
        return 0;
    }
    if (ctx.mode == ResultMode::stream) {
        for (size_t i = 0; i < count; ++i) print_result(results[i], ctx.csv);
    } else if (ctx.mode == ResultMode::all) {
        ctx.all.insert(ctx.all.end(), results, results + count);
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (ctx.top.size() < ctx.top_count) ctx.top.push(results[i]);
            else if (Better{}(results[i], ctx.top.top())) {
                ctx.top.pop();
                ctx.top.push(results[i]);
            }
        }
    }
    return 0;
}

void report_progress(void *opaque, uint64_t completed, uint64_t total) {
    auto &ctx = *static_cast<Context *>(opaque);
    if (!ctx.progress || total == 0) return;
    const double fraction = static_cast<double>(completed) / static_cast<double>(total);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - ctx.start).count();
    const double eta = completed ? seconds * static_cast<double>(total - completed) / completed : 0;
    std::fprintf(stderr, "\r[%5.1f%%] %llu/%llu elapsed %.1fs ETA %.1fs   ", fraction * 100.0,
        static_cast<unsigned long long>(completed), static_cast<unsigned long long>(total), seconds, eta);
    if (completed == total) std::fputc('\n', stderr);
}

int should_cancel(void *) { return interrupted.load(std::memory_order_relaxed) ? 1 : 0; }

template<class T> bool parse_integer(const char *text, T &out) {
    const char *end = text + std::strlen(text);
    auto [ptr, error] = std::from_chars(text, end, out);
    return error == std::errc{} && ptr == end;
}

void usage(const char *program) {
    std::fprintf(stderr,
        "usage: %s [OPTIONS] SEED RANGE [THRESHOLD]\n"
        "       %s biome-score [OPTIONS] SEED RANGE [THRESHOLD]\n"
        "       %s --tui\n"
        "  -j, --threads N             worker threads (0=auto)\n"
        "  -m, --backend auto|scalar|avx2|neon|cuda\n"
        "  -f, --format human|csv      output format\n"
        "  -u, --unordered             stream without sorting\n"
        "      --top K                 retain only the best K results\n"
        "      --spawn-y Y             biome/spawn feet Y (default -63)\n"
        "      --player-y Y            AFK player feet Y (default -38)\n"
        "  -q, --quiet                 disable progress\n"
        "  -b, --benchmark             report throughput, omit results\n"
        "      --tui                   open interactive terminal UI\n"
        "  -v, --version               print version\n"
        "  -h, --help                  print help\n", program, program, program);
}

bool parse_backend(const char *value, ss_backend &backend) {
    if (std::strcmp(value, "auto") == 0 || std::strcmp(value, "cpu") == 0) backend = SS_BACKEND_AUTO;
    else if (std::strcmp(value, "scalar") == 0) backend = SS_BACKEND_SCALAR;
    else if (std::strcmp(value, "avx2") == 0) backend = SS_BACKEND_AVX2;
    else if (std::strcmp(value, "neon") == 0) backend = SS_BACKEND_NEON;
    else if (std::strcmp(value, "cuda") == 0) backend = SS_BACKEND_CUDA;
    else return false;
    return true;
}
} // namespace

int run_command_line(int argc, char **argv) {
    interrupted.store(false, std::memory_order_relaxed);
    ss_search_options_v1 options{sizeof(options), 0, SS_BACKEND_AUTO, 1024};
    Context context;
    const bool biome_mode = argc > 1 && std::strcmp(argv[1], "biome-score") == 0;
    int32_t spawn_y = -63;
    int32_t player_y = -38;
    bool custom_y = false;
    if (biome_mode) {
        context.mode = ResultMode::top;
        context.top_count = 20;
    }
    std::vector<const char *> positional;
    for (int i = biome_mode ? 2 : 1; i < argc; ++i) {
        const char *argument = argv[i];
        auto value = [&]() -> const char * { return ++i < argc ? argv[i] : nullptr; };
        if (!std::strcmp(argument, "-h") || !std::strcmp(argument, "--help")) { usage(argv[0]); return 0; }
        if (!std::strcmp(argument, "-v") || !std::strcmp(argument, "--version")) { std::printf("slimeseeker %s\n", ss_version()); return 0; }
        if (!std::strcmp(argument, "--tui")) { std::fprintf(stderr, "--tui cannot be combined with other arguments\n"); return 1; }
        if (!std::strcmp(argument, "-q") || !std::strcmp(argument, "--quiet")) { context.progress = false; continue; }
        if (!std::strcmp(argument, "-u") || !std::strcmp(argument, "--unordered")) {
            if (biome_mode) { std::fprintf(stderr, "--unordered is unavailable for biome-score\n"); return 1; }
            context.mode = ResultMode::stream; continue;
        }
        if (!std::strcmp(argument, "-b") || !std::strcmp(argument, "--benchmark")) {
            if (biome_mode) { std::fprintf(stderr, "--benchmark is unavailable for biome-score\n"); return 1; }
            context.benchmark = true; context.progress = false; continue;
        }
        if (!std::strcmp(argument, "-j") || !std::strcmp(argument, "--threads")) {
            const char *v = value(); if (!v || !parse_integer(v, options.threads)) { usage(argv[0]); return 1; } continue;
        }
        if (!std::strcmp(argument, "-m") || !std::strcmp(argument, "--backend")) {
            const char *v = value(); if (!v || !parse_backend(v, options.backend)) { usage(argv[0]); return 1; } continue;
        }
        if (!std::strcmp(argument, "-f") || !std::strcmp(argument, "--format")) {
            const char *v = value(); if (!v || (std::strcmp(v, "human") && std::strcmp(v, "csv"))) { usage(argv[0]); return 1; }
            context.csv = std::strcmp(v, "csv") == 0; continue;
        }
        if (!std::strcmp(argument, "--top")) {
            const char *v = value(); if (!v || !parse_integer(v, context.top_count) || context.top_count == 0) { usage(argv[0]); return 1; }
            context.mode = ResultMode::top; continue;
        }
        if (!std::strcmp(argument, "--spawn-y")) {
            const char *v = value(); if (!v || !parse_integer(v, spawn_y)) { usage(argv[0]); return 1; }
            custom_y = true; continue;
        }
        if (!std::strcmp(argument, "--player-y")) {
            const char *v = value(); if (!v || !parse_integer(v, player_y)) { usage(argv[0]); return 1; }
            custom_y = true; continue;
        }
        if (argument[0] == '-' && !(argument[1] >= '0' && argument[1] <= '9')) {
            std::fprintf(stderr, "unknown option: %s\n", argument); return 1;
        }
        positional.push_back(argument);
    }
    if (positional.size() < 2 || positional.size() > 3) { usage(argv[0]); return 1; }
    int64_t seed = 0, range = 0;
    unsigned parsed_threshold = 45;
    if (!parse_integer(positional[0], seed) || !parse_integer(positional[1], range) || range <= 0 ||
        (positional.size() == 3 && (!parse_integer(positional[2], parsed_threshold) || parsed_threshold > SS_DONUT_CELLS))) {
        std::fprintf(stderr, "invalid SEED, RANGE, or THRESHOLD\n"); return 1;
    }
    if (range > static_cast<int64_t>(std::numeric_limits<int32_t>::max()) - 8) {
        std::fprintf(stderr, "RANGE is too large\n"); return 1;
    }
    if (biome_mode && range > (static_cast<int64_t>(std::numeric_limits<int32_t>::max()) - 15) / 16 - 8) {
        std::fprintf(stderr, "RANGE is too large for block-coordinate biome sampling\n"); return 1;
    }
    if (biome_mode && (spawn_y < -64 || spawn_y > 319 || player_y < -64 || player_y > 319)) {
        std::fprintf(stderr, "spawn and player Y must be within the 26.2 overworld build range [-64,319]\n"); return 1;
    }
    if (!biome_mode && custom_y) {
        std::fprintf(stderr, "--spawn-y and --player-y require biome-score\n"); return 1;
    }
    if (biome_mode) {
        try {
            context.biome_scorer = std::make_unique<BiomeScorer>(seed, context.top_count, spawn_y, player_y);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "cannot initialize biome scoring: %s\n", error.what());
            return 1;
        }
    }
    ss_search_params_v1 params{sizeof(params), seed, static_cast<int32_t>(-range), static_cast<int32_t>(range),
                               static_cast<int32_t>(-range), static_cast<int32_t>(range),
                               static_cast<uint16_t>(parsed_threshold), 0};
    ss_callbacks_v1 callbacks{sizeof(callbacks), &context, receive_results, report_progress, should_cancel};
    std::signal(SIGINT, [](int) { interrupted.store(true, std::memory_order_relaxed); });
    context.start = std::chrono::steady_clock::now();
    const ss_status status = ss_search(&params, &options, &callbacks);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - context.start).count();
    if (status != SS_OK) {
        if (!context.callback_error.empty()) std::fprintf(stderr, "biome scoring failed: %s\n", context.callback_error.c_str());
        else std::fprintf(stderr, "search failed: %s\n", ss_status_string(status));
        return status;
    }

    if (biome_mode) {
        std::vector<BiomeRankedResult> results;
        try {
            results = context.biome_scorer->finish();
        } catch (const std::exception &error) {
            std::fprintf(stderr, "cannot finish biome scoring: %s\n", error.what());
            return 1;
        }
        if (context.csv)
            std::puts("rank,x,z,count,biome_score,common_equivalent_chunks,player_x,player_y,player_z,afk_score");
        for (size_t i = 0; i < results.size(); ++i) {
            const auto &result = results[i];
            if (context.csv) {
                std::printf("%zu,%d,%d,%u,%.9f,%.9f,%.1f,%d,%.1f,%.9f\n", i + 1,
                    result.source.x, result.source.z, result.source.count, result.biome_score,
                    result.common_equivalent_chunks, result.player_x, result.player_y, result.player_z, result.afk_score);
            } else {
                std::printf("%zu. (%d, %d) count=%u biome=%.6f common_chunks=%.6f AFK=(%.1f, %d, %.1f) afk=%.6f\n",
                    i + 1, result.source.x, result.source.z, result.source.count, result.biome_score,
                    result.common_equivalent_chunks, result.player_x, result.player_y, result.player_z, result.afk_score);
            }
        }
        return 0;
    }

    if (context.benchmark) {
        const uint64_t side = static_cast<uint64_t>(range) * 2;
        const uint64_t candidates = side * side;
        std::printf("backend_requested: %s\nthreads: %u\ncandidates: %llu\nelapsed_s: %.6f\ncandidates_per_s: %.3f\n",
            ss_backend_name(options.backend), options.threads, static_cast<unsigned long long>(candidates), seconds,
            static_cast<double>(candidates) / seconds);
        return 0;
    }
    if (context.mode == ResultMode::all) {
        std::sort(context.all.begin(), context.all.end(), Better{});
        for (const auto &result : context.all) print_result(result, context.csv);
    } else if (context.mode == ResultMode::top) {
        context.all.reserve(context.top.size());
        while (!context.top.empty()) { context.all.push_back(context.top.top()); context.top.pop(); }
        std::sort(context.all.begin(), context.all.end(), Better{});
        for (const auto &result : context.all) print_result(result, context.csv);
    }
    return 0;
}
} // namespace ss::cli
