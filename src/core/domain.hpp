/*
 * Minecraft 史莱姆区块领域模型内部接口。
 *
 * 集中定义定宽回绕、Java LCG 常量、区块种子分解和圆环几何。该头文件仅供项目内部
 * 使用；稳定的对外接口位于 include/slimeseeker/slimeseeker.h。
 */
#pragma once

#include <array>
#include <cstdint>
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

// 该函数位于头文件中，保证每个区块都会调用的 LCG 热路径可被后端直接内联。
// rejection 尾部概率极低，但必须保留循环以逐位兼容 java.util.Random.nextInt(10)。
inline bool is_slime_from_chunk_seed(int64_t chunk_seed_value) {
    uint64_t state = (static_cast<uint64_t>(chunk_seed_value) ^ kLcgMul) & kLcgMask;
    for (;;) {
        state = (state * kLcgMul + kLcgAdd) & kLcgMask;
        const uint32_t bits = static_cast<uint32_t>(state >> 17);
        const uint32_t value = bits % 10u;
        if (bits - value + 9u < 0x80000000u) return value == 0;
    }
}
inline bool is_slime(int64_t seed, int32_t x, int32_t z) {
    return is_slime_from_chunk_seed(chunk_seed(seed, x, z));
}
const Runs &donut_runs();
bool in_donut(int dx, int dz);

} // namespace ss
