#include "slimeseeker/slimeseeker.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <limits>
#include <queue>
#include <string>
#include <vector>

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
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

void print_result(const ss_result &result, bool csv) {
    if (csv) std::printf("%d,%d,%u\n", result.x, result.z, result.count);
    else std::printf("(%d, %d)   %u\n", result.x, result.z, result.count);
}

int receive_results(void *opaque, const ss_result *results, size_t count) {
    auto &ctx = *static_cast<Context *>(opaque);
    if (ctx.benchmark) return 0;
    if (ctx.mode == ResultMode::stream) {
        for (size_t i = 0; i < count; ++i) print_result(results[i], ctx.csv);
    } else if (ctx.mode == ResultMode::all) {
        ctx.all.insert(ctx.all.end(), results, results + count);
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (ctx.top.size() < ctx.top_count) ctx.top.push(results[i]);
            else if (Better{}(results[i], ctx.top.top())) { ctx.top.pop(); ctx.top.push(results[i]); }
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
        "  -j, --threads N             worker threads (0=auto)\n"
        "  -m, --backend auto|scalar|avx2|neon\n"
        "  -f, --format human|csv      output format\n"
        "  -u, --unordered             stream without sorting\n"
        "      --top K                 retain only the best K results\n"
        "  -q, --quiet                 disable progress\n"
        "  -b, --benchmark             report throughput, omit results\n"
        "  -v, --version               print version\n"
        "  -h, --help                  print help\n", program);
}

bool parse_backend(const char *value, ss_backend &backend) {
    if (std::strcmp(value, "auto") == 0 || std::strcmp(value, "cpu") == 0) backend = SS_BACKEND_AUTO;
    else if (std::strcmp(value, "scalar") == 0) backend = SS_BACKEND_SCALAR;
    else if (std::strcmp(value, "avx2") == 0) backend = SS_BACKEND_AVX2;
    else if (std::strcmp(value, "neon") == 0) backend = SS_BACKEND_NEON;
    else return false;
    return true;
}
} // namespace

int main(int argc, char **argv) {
    ss_search_options_v1 options{sizeof(options), 0, SS_BACKEND_AUTO, 1024};
    Context context;
    std::vector<const char *> positional;
    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        auto value = [&]() -> const char * { return ++i < argc ? argv[i] : nullptr; };
        if (!std::strcmp(argument, "-h") || !std::strcmp(argument, "--help")) { usage(argv[0]); return 0; }
        if (!std::strcmp(argument, "-v") || !std::strcmp(argument, "--version")) { std::printf("slimeseeker %s\n", ss_version()); return 0; }
        if (!std::strcmp(argument, "-q") || !std::strcmp(argument, "--quiet")) { context.progress = false; continue; }
        if (!std::strcmp(argument, "-u") || !std::strcmp(argument, "--unordered")) { context.mode = ResultMode::stream; continue; }
        if (!std::strcmp(argument, "-b") || !std::strcmp(argument, "--benchmark")) { context.benchmark = true; context.progress = false; continue; }
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
        if (argument[0] == '-' && !(argument[1] >= '0' && argument[1] <= '9')) {
            std::fprintf(stderr, "unknown option: %s\n", argument); return 1;
        }
        positional.push_back(argument);
    }
    if (positional.size() < 2 || positional.size() > 3) { usage(argv[0]); return 1; }
    int64_t seed = 0, range = 0;
    uint16_t threshold = 45;
    unsigned parsed_threshold = 45;
    if (!parse_integer(positional[0], seed) || !parse_integer(positional[1], range) || range <= 0 ||
        (positional.size() == 3 && (!parse_integer(positional[2], parsed_threshold) || parsed_threshold > SS_DONUT_CELLS))) {
        std::fprintf(stderr, "invalid SEED, RANGE, or THRESHOLD\n"); return 1;
    }
    if (range > static_cast<int64_t>(std::numeric_limits<int32_t>::max()) - 8) {
        std::fprintf(stderr, "RANGE is too large\n"); return 1;
    }
    threshold = static_cast<uint16_t>(parsed_threshold);
    ss_search_params_v1 params{sizeof(params), seed, static_cast<int32_t>(-range), static_cast<int32_t>(range),
                               static_cast<int32_t>(-range), static_cast<int32_t>(range), threshold, 0};
    ss_callbacks_v1 callbacks{sizeof(callbacks), &context, receive_results, report_progress, should_cancel};
    std::signal(SIGINT, [](int) { interrupted.store(true, std::memory_order_relaxed); });
    context.start = std::chrono::steady_clock::now();
    const ss_status status = ss_search(&params, &options, &callbacks);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - context.start).count();
    if (status != SS_OK) { std::fprintf(stderr, "search failed: %s\n", ss_status_string(status)); return status; }

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
