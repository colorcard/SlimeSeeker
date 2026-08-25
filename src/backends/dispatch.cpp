/*
 * CPU 算子能力检测与运行时分派。
 *
 * 显式后端只检查硬件支持；auto 还会对拍并校准性能，避免“支持 SIMD”却实际降速。
 */
#include "backends/backend.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace ss {

bool cpu_has_avx2() {
#if defined(SS_HAS_AVX2_OBJECT) && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#elif defined(SS_HAS_AVX2_OBJECT) && defined(_MSC_VER)
    int regs[4]{};
    __cpuid(regs, 1);
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avx = (regs[2] & (1 << 28)) != 0;
    if (!osxsave || !avx || (_xgetbv(0) & 6) != 6) return false;
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0;
#else
    return false;
#endif
}

bool cpu_has_neon() {
#if defined(SS_HAS_NEON_OBJECT) && (defined(__aarch64__) || defined(_M_ARM64))
    return true;
#else
    return false;
#endif
}

#if !defined(SS_HAS_AVX2_OBJECT)
void build_map_avx2(int64_t s, int32_t x, int32_t z, int w, int h, uint8_t *o) {
    build_map_scalar(s, x, z, w, h, o);
}
#endif
#if !defined(SS_HAS_NEON_OBJECT)
void build_map_neon(int64_t s, int32_t x, int32_t z, int w, int h, uint8_t *o) {
    build_map_scalar(s, x, z, w, h, o);
}
#endif

static BuildMapFn calibrated(BuildMapFn candidate) {
    // 指令集可用不等于一定更快：AVX2/NEON 对 64 位 LCG 乘法的收益高度依赖 CPU。
    // 用固定输入先对拍，再以中位数过滤冷启动抖动；不足 5% 的收益仍选择标量。
    constexpr int width = 256, height = 128;
    std::vector<uint8_t> baseline(width * height), trial(width * height);
    using clock = std::chrono::steady_clock;
    auto measure = [&](BuildMapFn fn) {
        std::array<int64_t, 7> samples{};
        for (auto &sample : samples) {
            const auto start = clock::now();
            fn(0, -12345, 6789, width, height, trial.data());
            sample = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
        }
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    };
    build_map_scalar(0, -12345, 6789, width, height, baseline.data());
    candidate(0, -12345, 6789, width, height, trial.data());
    if (baseline != trial) return build_map_scalar;
    const auto scalar_ns = measure(build_map_scalar);
    const auto candidate_ns = measure(candidate);
    return candidate_ns * 100 < scalar_ns * 95 ? candidate : build_map_scalar;
}

BuildMapFn select_backend(ss_backend requested, ss_backend &selected) {
    // 显式选择只做能力检查，不做性能门槛判断，便于测试与可复现基准。
    if (requested == SS_BACKEND_SCALAR) { selected = SS_BACKEND_SCALAR; return build_map_scalar; }
    if (requested == SS_BACKEND_AVX2) {
        if (!cpu_has_avx2()) return nullptr;
        selected = SS_BACKEND_AVX2; return build_map_avx2;
    }
    if (requested == SS_BACKEND_NEON) {
        if (!cpu_has_neon()) return nullptr;
        selected = SS_BACKEND_NEON; return build_map_neon;
    }
    if (requested != SS_BACKEND_AUTO) return nullptr;

    // 自动校准每个进程只执行一次，后续搜索直接复用选择结果。
    static std::once_flag once;
    static BuildMapFn choice = build_map_scalar;
    static ss_backend choice_name = SS_BACKEND_SCALAR;
    std::call_once(once, [] {
        if (cpu_has_avx2()) {
            choice = calibrated(build_map_avx2);
            if (choice == build_map_avx2) choice_name = SS_BACKEND_AVX2;
        } else if (cpu_has_neon()) {
            choice = calibrated(build_map_neon);
            if (choice == build_map_neon) choice_name = SS_BACKEND_NEON;
        }
    });
    selected = choice_name;
    return choice;
}

} // namespace ss
