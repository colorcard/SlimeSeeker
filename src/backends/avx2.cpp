/*
 * x86-64 AVX2 位图算子。
 *
 * 本文件作为独立编译单元启用 AVX2，只向量化可获益且有原生支持的 32 位回绕运算；
 * 精确 LCG 判定继续复用领域核心，保证冷门 rejection 分支不产生语义偏差。
 */
#include "backends/backend.hpp"
#include "core/domain.hpp"
#include <immintrin.h>
#include <vector>

namespace ss {

void build_map_avx2(int64_t seed, int32_t x0, int32_t z0, int width, int height, uint8_t *out) {
    std::vector<uint64_t> xt(static_cast<size_t>(width));
    int x = 0;
    const __m256i offsets = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
    const __m256i c1 = _mm256_set1_epi32(4987142);
    const __m256i c2 = _mm256_set1_epi32(5947611);
    alignas(32) int32_t terms_a[8], terms_b[8];
    for (; x + 8 <= width; x += 8) {
        // 只向量化有原生支持的 8×i32 回绕乘法；LCG 的精确 rejection 仍复用标量黄金实现。
        const __m256i vx = _mm256_add_epi32(_mm256_set1_epi32(x0 + x), offsets);
        const __m256i t1 = _mm256_mullo_epi32(_mm256_mullo_epi32(vx, vx), c1);
        const __m256i t2 = _mm256_mullo_epi32(vx, c2);
        _mm256_store_si256(reinterpret_cast<__m256i *>(terms_a), t1);
        _mm256_store_si256(reinterpret_cast<__m256i *>(terms_b), t2);
        for (int lane = 0; lane < 8; ++lane) {
            xt[static_cast<size_t>(x + lane)] = static_cast<uint64_t>(static_cast<int64_t>(terms_a[lane]))
                + static_cast<uint64_t>(static_cast<int64_t>(terms_b[lane]));
        }
    }
    for (; x < width; ++x) xt[static_cast<size_t>(x)] = xterm(x0 + x);
    for (int z = 0; z < height; ++z) {
        const uint64_t zb = zbase(seed, z0 + z);
        auto *row = out + static_cast<size_t>(z) * static_cast<size_t>(width);
        for (int column = 0; column < width; ++column)
            row[column] = static_cast<uint8_t>(is_slime_from_chunk_seed(
                static_cast<int64_t>((zb + xt[static_cast<size_t>(column)]) ^ kChunkXor)));
    }
}

} // namespace ss
