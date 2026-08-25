#include "internal.hpp"
#include "slimeseeker/slimeseeker.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; } } while (0)

bool result_less(const ss_result &a, const ss_result &b) {
    if (a.z != b.z) return a.z < b.z;
    if (a.x != b.x) return a.x < b.x;
    return a.count < b.count;
}

unsigned naive_count(int64_t seed, int32_t x, int32_t z) {
    unsigned count = 0;
    for (int dz = -8; dz <= 8; ++dz)
        for (int dx = -8; dx <= 8; ++dx)
            if (ss::in_donut(dx, dz)) count += ss::is_slime(seed, x + dx, z + dz);
    return count;
}

int collect(void *opaque, const ss_result *results, size_t count) {
    auto &out = *static_cast<std::vector<ss_result> *>(opaque);
    out.insert(out.end(), results, results + count);
    return 0;
}

std::vector<ss_result> run(int64_t seed, int32_t x0, int32_t x1, int32_t z0, int32_t z1,
                           uint16_t threshold, unsigned threads, ss_backend backend) {
    ss_search_params_v1 params{sizeof(params), seed, x0, x1, z0, z1, threshold, 0};
    ss_search_options_v1 options{sizeof(options), threads, backend, 17};
    std::vector<ss_result> results;
    ss_callbacks_v1 callbacks{sizeof(callbacks), &results, collect, nullptr, nullptr};
    CHECK(ss_search(&params, &options, &callbacks) == SS_OK);
    std::sort(results.begin(), results.end(), result_less);
    return results;
}

void test_geometry() {
    int cells = 0;
    for (int z = -8; z <= 8; ++z) for (int x = -8; x <= 8; ++x) cells += ss::in_donut(x, z);
    CHECK(cells == 192);
    const auto &runs = ss::donut_runs();
    CHECK(runs.size() == 20);
    int run_cells = 0;
    for (const auto &run : runs) run_cells += run.last - run.first + 1;
    CHECK(run_cells == cells);
}

void test_seed_decomposition() {
    std::mt19937_64 random(0x51A1EULL);
    for (int i = 0; i < 10000; ++i) {
        const int64_t seed = static_cast<int64_t>(random());
        const int32_t x = static_cast<int32_t>(random());
        const int32_t z = static_cast<int32_t>(random());
        const int32_t t1 = ss::mul32(ss::mul32(x, x), 4987142);
        const int32_t t2 = ss::mul32(x, 5947611);
        const int64_t t3 = static_cast<int64_t>(ss::mul32(z, z)) * 4392871LL;
        const int32_t t4 = ss::mul32(z, 389711);
        uint64_t expected = static_cast<uint64_t>(seed);
        expected += static_cast<uint64_t>(static_cast<int64_t>(t1));
        expected += static_cast<uint64_t>(static_cast<int64_t>(t2));
        expected += static_cast<uint64_t>(t3);
        expected += static_cast<uint64_t>(static_cast<int64_t>(t4));
        expected ^= ss::kChunkXor;
        CHECK(static_cast<uint64_t>(ss::chunk_seed(seed, x, z)) == expected);
    }
}

void test_differential_search() {
    constexpr int64_t seed = -184718958561915LL;
    auto actual = run(seed, -19, 23, -17, 21, 20, 3, SS_BACKEND_SCALAR);
    std::vector<ss_result> expected;
    for (int32_t z = -17; z < 21; ++z) for (int32_t x = -19; x < 23; ++x) {
        const auto count = naive_count(seed, x, z);
        if (count >= 20) expected.push_back({x, z, static_cast<uint16_t>(count), 0});
    }
    std::sort(expected.begin(), expected.end(), result_less);
    CHECK(actual.size() == expected.size());
    CHECK(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end(), [](auto a, auto b) {
        return a.x == b.x && a.z == b.z && a.count == b.count;
    }));
}

void test_threads_and_backends() {
    const auto scalar = run(5023147298867078368LL, -260, 271, -253, 267, 35, 1, SS_BACKEND_SCALAR);
    const auto parallel = run(5023147298867078368LL, -260, 271, -253, 267, 35, 4, SS_BACKEND_SCALAR);
    CHECK(scalar.size() == parallel.size());
    CHECK(std::equal(scalar.begin(), scalar.end(), parallel.begin(), parallel.end(), [](auto a, auto b) {
        return a.x == b.x && a.z == b.z && a.count == b.count;
    }));
    for (ss_backend backend : {SS_BACKEND_AVX2, SS_BACKEND_NEON}) {
        if (!ss_backend_available(backend)) continue;
        const auto specialized = run(5023147298867078368LL, -260, 271, -253, 267, 35, 2, backend);
        CHECK(scalar.size() == specialized.size());
        CHECK(std::equal(scalar.begin(), scalar.end(), specialized.begin(), specialized.end(), [](auto a, auto b) {
            return a.x == b.x && a.z == b.z && a.count == b.count;
        }));
    }
}

void test_api_validation() {
    ss_search_params_v1 params{sizeof(params), 0, -1, 1, -1, 1, 193, 0};
    CHECK(ss_search(&params, nullptr, nullptr) == SS_INVALID_ARGUMENT);
    params.threshold = 0; params.x_begin = 2; params.x_end = 1;
    CHECK(ss_search(&params, nullptr, nullptr) == SS_INVALID_ARGUMENT);
    CHECK(std::string(ss_version()) == "1.0.0");
}

struct ControlContext {
    uint64_t last = 0;
    bool monotonic = true;
    int batches = 0;
};
int abort_results(void *opaque, const ss_result *, size_t) {
    auto &ctx = *static_cast<ControlContext *>(opaque);
    ++ctx.batches;
    return 1;
}
void progress(void *opaque, uint64_t completed, uint64_t) {
    auto &ctx = *static_cast<ControlContext *>(opaque);
    if (completed < ctx.last) ctx.monotonic = false;
    ctx.last = completed;
}
int cancel_now(void *) { return 1; }

void test_control_contracts() {
    ss_search_params_v1 params{sizeof(params), 0, -100, 100, -100, 100, 0, 0};
    ss_search_options_v1 options{sizeof(options), 2, SS_BACKEND_SCALAR, 8};
    ControlContext ctx;
    ss_callbacks_v1 callbacks{sizeof(callbacks), &ctx, abort_results, progress, nullptr};
    CHECK(ss_search(&params, &options, &callbacks) == SS_CALLBACK_ABORTED);
    CHECK(ctx.batches == 1);
    CHECK(ctx.monotonic);
    callbacks.on_results = nullptr;
    callbacks.should_cancel = cancel_now;
    CHECK(ss_search(&params, &options, &callbacks) == SS_CANCELLED);
}
} // namespace

int main() {
    test_geometry();
    test_seed_decomposition();
    test_differential_search();
    test_threads_and_backends();
    test_api_validation();
    test_control_contracts();
    if (failures) std::fprintf(stderr, "%d test checks failed\n", failures);
    else std::printf("all tests passed\n");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
