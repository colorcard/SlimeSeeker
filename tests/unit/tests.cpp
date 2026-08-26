/*
 * 核心单元与差分测试入口。
 *
 * 使用独立朴素计数验证领域公式、搜索结果、线程一致性、后端一致性及控制回调契约。
 */
#include "core/domain.hpp"
#include "slimeseeker/slimeseeker.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; } } while (0)

bool result_less(const ss_result &a, const ss_result &b) {
    if (a.count != b.count) return a.count > b.count;
    const uint64_t da = static_cast<uint64_t>(static_cast<int64_t>(a.x) * a.x) +
                        static_cast<uint64_t>(static_cast<int64_t>(a.z) * a.z);
    const uint64_t db = static_cast<uint64_t>(static_cast<int64_t>(b.x) * b.x) +
                        static_cast<uint64_t>(static_cast<int64_t>(b.z) * b.z);
    if (da != db) return da < db;
    if (a.x != b.x) return a.x < b.x;
    return a.z < b.z;
}

bool same_results(const std::vector<ss_result> &a, const std::vector<ss_result> &b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto &x, const auto &y) {
               return x.x == y.x && x.z == y.z && x.count == y.count;
           });
}

unsigned naive_count(int64_t seed, int32_t x, int32_t z) {
    unsigned count = 0;
    for (int dz = -8; dz <= 8; ++dz)
        for (int dx = -8; dx <= 8; ++dx)
            if (ss::in_donut(dx, dz)) count += ss::is_slime(seed, x + dx, z + dz);
    return count;
}

bool reference_slime_from_chunk_seed(int64_t chunk_seed) {
    uint64_t state = (static_cast<uint64_t>(chunk_seed) ^ ss::kLcgMul) & ss::kLcgMask;
    for (;;) {
        state = (state * ss::kLcgMul + ss::kLcgAdd) & ss::kLcgMask;
        const uint32_t bits = static_cast<uint32_t>(state >> 17);
        const uint32_t value = bits % 10u;
        if (bits - value + 9u < 0x80000000u) return value == 0;
    }
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

void test_next_int10_rejection_path() {
    // 逆向构造第一次输出为拒绝区间起点的状态，确保冷路径不是仅靠随机测试覆盖。
    constexpr uint64_t inverse_multiplier = 0xDFE05BCB1365ULL;
    constexpr uint64_t rejected_next_state = static_cast<uint64_t>(ss::kNextInt10Limit) << 17;
    constexpr uint64_t initial_state =
        ((rejected_next_state - ss::kLcgAdd) * inverse_multiplier) & ss::kLcgMask;
    constexpr int64_t chunk_seed = static_cast<int64_t>(initial_state ^ ss::kLcgMul);
    CHECK(ss::is_slime_from_chunk_seed(chunk_seed) == reference_slime_from_chunk_seed(chunk_seed));

    std::mt19937_64 random(0x10EAC7ULL);
    for (int i = 0; i < 10000; ++i) {
        const auto value = static_cast<int64_t>(random());
        CHECK(ss::is_slime_from_chunk_seed(value) == reference_slime_from_chunk_seed(value));
    }
}

void test_differential_search() {
    constexpr int64_t seed = -184718958561915LL;
    for (uint16_t threshold : {uint16_t{20}, uint16_t{30}, uint16_t{31}, uint16_t{40}, uint16_t{41}}) {
        auto actual = run(seed, -19, 23, -17, 21, threshold, 3, SS_BACKEND_SCALAR);
        std::vector<ss_result> expected;
        for (int32_t z = -17; z < 21; ++z) for (int32_t x = -19; x < 23; ++x) {
            const auto count = naive_count(seed, x, z);
            if (count >= threshold) expected.push_back({x, z, static_cast<uint16_t>(count), 0});
        }
        std::sort(expected.begin(), expected.end(), result_less);
        CHECK(actual.size() == expected.size());
        CHECK(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end(), [](auto a, auto b) {
            return a.x == b.x && a.z == b.z && a.count == b.count;
        }));
    }
}

void test_threads_and_backends() {
    const auto scalar = run(5023147298867078368LL, -260, 271, -253, 267, 35, 1, SS_BACKEND_SCALAR);
    const auto parallel = run(5023147298867078368LL, -260, 271, -253, 267, 35, 4, SS_BACKEND_SCALAR);
    CHECK(same_results(scalar, parallel));
    for (ss_backend backend : {SS_BACKEND_AVX2, SS_BACKEND_NEON}) {
        if (!ss_backend_available(backend)) continue;
        const auto specialized = run(5023147298867078368LL, -260, 271, -253, 267, 35, 2, backend);
        CHECK(same_results(scalar, specialized));
    }
}

void test_api_validation() {
    ss_search_params_v1 params{sizeof(params), 0, -1, 1, -1, 1, 193, 0};
    CHECK(ss_search(&params, nullptr, nullptr) == SS_INVALID_ARGUMENT);
    params.threshold = 0; params.x_begin = 2; params.x_end = 1;
    CHECK(ss_search(&params, nullptr, nullptr) == SS_INVALID_ARGUMENT);
    CHECK(std::string(ss_version()) == "1.1.0");
}

struct ControlContext {
    uint64_t last = 0;
    uint64_t total = 0;
    bool monotonic = true;
    bool valid_progress = true;
    int batches = 0;
    int progress_calls = 0;
    std::vector<ss_result> results;
};
int abort_results(void *opaque, const ss_result *, size_t) {
    auto &ctx = *static_cast<ControlContext *>(opaque);
    ++ctx.batches;
    return 1;
}
void progress(void *opaque, uint64_t completed, uint64_t) {
    auto &ctx = *static_cast<ControlContext *>(opaque);
    ++ctx.progress_calls;
    if (completed < ctx.last) ctx.monotonic = false;
    ctx.last = completed;
}
int cancel_now(void *) { return 1; }

int collect_controlled(void *opaque, const ss_result *results, size_t count) {
    auto &ctx = *static_cast<ControlContext *>(opaque);
    ++ctx.batches;
    ctx.results.insert(ctx.results.end(), results, results + count);
    return 0;
}

void contract_progress(void *opaque, uint64_t completed, uint64_t total) {
    auto &ctx = *static_cast<ControlContext *>(opaque);
    ++ctx.progress_calls;
    if (completed < ctx.last || completed > total) ctx.monotonic = false;
    if (ctx.total != 0 && ctx.total != total) ctx.valid_progress = false;
    ctx.last = completed;
    ctx.total = total;
}

ss_status run_controlled(const ss_search_params_v1 &params, ss_backend backend,
                         ControlContext &ctx, ss_results_fn on_results,
                         ss_progress_fn on_progress, ss_cancel_fn should_cancel) {
    ss_search_options_v1 options{sizeof(options), 2, backend, 31};
    ss_callbacks_v1 callbacks{
        sizeof(callbacks), &ctx, on_results, on_progress, should_cancel};
    return ss_search(&params, &options, &callbacks);
}

void test_search_backend_contract(ss_backend backend) {
    if (!ss_backend_available(backend)) return;
    constexpr int64_t seed = -184718958561915LL;

    // 空区域仍须成功，并报告确定的 0/0 进度。
    ss_search_params_v1 empty{sizeof(empty), seed, -7, -7, -9, 12, 45, 0};
    ControlContext empty_ctx;
    CHECK(run_controlled(empty, backend, empty_ctx, collect_controlled,
                         contract_progress, nullptr) == SS_OK);
    CHECK(empty_ctx.results.empty());
    CHECK(empty_ctx.progress_calls == 1);
    CHECK(empty_ctx.last == 0 && empty_ctx.total == 0);
    CHECK(empty_ctx.monotonic && empty_ctx.valid_progress);

    // 负坐标与三条 CPU 阈值管线边界都必须和 scalar 黄金后端逐项一致。
    for (uint16_t threshold : {uint16_t{0}, uint16_t{30}, uint16_t{45}, uint16_t{192}}) {
        const auto expected = run(seed, -37, 29, -31, 23, threshold, 1, SS_BACKEND_SCALAR);
        const auto actual = run(seed, -37, 29, -31, 23, threshold, 2, backend);
        CHECK(same_results(expected, actual));
    }

    // 阈值 0 填满一个 496x496 tile 并跨越双轴边界，验证结果不会因设备缓冲而截断。
    ss_search_params_v1 dense{sizeof(dense), seed, -37, 521, -29, 509, 0, 0};
    const auto expected = run(seed, dense.x_begin, dense.x_end, dense.z_begin, dense.z_end,
                              dense.threshold, 1, SS_BACKEND_SCALAR);
    ControlContext dense_ctx;
    CHECK(run_controlled(dense, backend, dense_ctx, collect_controlled,
                         contract_progress, nullptr) == SS_OK);
    std::sort(dense_ctx.results.begin(), dense_ctx.results.end(), result_less);
    const uint64_t dense_total =
        static_cast<uint64_t>(dense.x_end - dense.x_begin) * (dense.z_end - dense.z_begin);
    CHECK(dense_ctx.results.size() == dense_total);
    CHECK(same_results(expected, dense_ctx.results));
    CHECK(dense_ctx.last == dense_total && dense_ctx.total == dense_total);
    CHECK(dense_ctx.progress_calls > 0);
    CHECK(dense_ctx.monotonic && dense_ctx.valid_progress);

    ss_search_params_v1 controlled{sizeof(controlled), seed, -100, 100, -100, 100, 0, 0};
    ControlContext abort_ctx;
    CHECK(run_controlled(controlled, backend, abort_ctx, abort_results,
                         contract_progress, nullptr) == SS_CALLBACK_ABORTED);
    CHECK(abort_ctx.batches == 1);
    CHECK(abort_ctx.monotonic && abort_ctx.valid_progress);

    ControlContext cancel_ctx;
    CHECK(run_controlled(controlled, backend, cancel_ctx, nullptr,
                         contract_progress, cancel_now) == SS_CANCELLED);
    CHECK(cancel_ctx.results.empty());
}

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
    test_next_int10_rejection_path();
    test_differential_search();
    test_threads_and_backends();
    test_api_validation();
    test_control_contracts();
    test_search_backend_contract(SS_BACKEND_SCALAR);
    test_search_backend_contract(SS_BACKEND_CUDA);
    if (failures) std::fprintf(stderr, "%d test checks failed\n", failures);
    else std::printf("all tests passed\n");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
