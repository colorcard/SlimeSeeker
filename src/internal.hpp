#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>
#include "slimeseeker/slimeseeker.h"

namespace ss {

constexpr int kRadius = 8;
constexpr int kWindow = 17;
constexpr int kTileMap = 512;
constexpr int kTileCenters = kTileMap - kWindow + 1;
constexpr uint64_t kLcgMul = 0x5DEECE66DULL;
constexpr uint64_t kLcgAdd = 0xBULL;
constexpr uint64_t kLcgMask = (1ULL << 48) - 1;
constexpr uint64_t kChunkXor = 987234911ULL;

// 17×17 圆环按“行内连续区间”压缩为 20 段，SAT 查询时每段只需四次读取。
struct Run { uint8_t row, first, last; };
using Runs = std::array<Run, 20>;

// Java 原实现中的 int 乘法按 32 位二进制补码回绕。
// 这里先转无符号执行运算，避免 C++ 有符号溢出的未定义行为。
constexpr int32_t mul32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
constexpr uint64_t xterm(int32_t x) {
    const int32_t a = mul32(mul32(x, x), 4987142);
    const int32_t b = mul32(x, 5947611);
    return static_cast<uint64_t>(static_cast<int64_t>(a)) + static_cast<uint64_t>(static_cast<int64_t>(b));
}
// chunk seed 在最终异或之前可拆成仅依赖 x 和仅依赖 seed/z 的两项；
// tile 内分别预计算后，可消除每个区块重复执行的多次乘法。
constexpr uint64_t zbase(int64_t seed, int32_t z) {
    const int64_t a = static_cast<int64_t>(mul32(z, z)) * 4392871LL;
    const int32_t b = mul32(z, 389711);
    return static_cast<uint64_t>(seed) + static_cast<uint64_t>(a) + static_cast<uint64_t>(static_cast<int64_t>(b));
}
constexpr int64_t chunk_seed(int64_t seed, int32_t x, int32_t z) {
    return static_cast<int64_t>((zbase(seed, z) + xterm(x)) ^ kChunkXor);
}
bool is_slime_from_chunk_seed(int64_t seed);
inline bool is_slime(int64_t seed, int32_t x, int32_t z) { return is_slime_from_chunk_seed(chunk_seed(seed, x, z)); }
const Runs &donut_runs();
bool in_donut(int dx, int dz);

using BuildMapFn = void (*)(int64_t, int32_t, int32_t, int, int, uint8_t *);
void build_map_scalar(int64_t, int32_t, int32_t, int, int, uint8_t *);
void build_map_avx2(int64_t, int32_t, int32_t, int, int, uint8_t *);
void build_map_neon(int64_t, int32_t, int32_t, int, int, uint8_t *);
bool cpu_has_avx2();
bool cpu_has_neon();
BuildMapFn select_backend(ss_backend requested, ss_backend &selected);

ss_status search_impl(const ss_search_params_v1 &, const ss_search_options_v1 &, const ss_callbacks_v1 &);

} // namespace ss
