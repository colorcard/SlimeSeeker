#include "internal.hpp"
#include <arm_neon.h>

namespace ss {

void build_map_neon(int64_t seed, int32_t x0, int32_t z0, int width, int height, uint8_t *out) {
    std::vector<uint64_t> xt(static_cast<size_t>(width));
    int x = 0;
    const int32x4_t offsets = {0, 1, 2, 3};
    for (; x + 4 <= width; x += 4) {
        // arm64 NEON 批量生成四列 xterm，并分别符号扩展两项以保持 Java int 语义。
        const int32x4_t vx = vaddq_s32(vdupq_n_s32(x0 + x), offsets);
        const int32x4_t square = vmulq_s32(vx, vx);
        const int32x4_t a = vmulq_n_s32(square, 4987142);
        const int32x4_t b = vmulq_n_s32(vx, 5947611);
        alignas(16) int32_t va[4], vb[4];
        vst1q_s32(va, a); vst1q_s32(vb, b);
        for (int lane = 0; lane < 4; ++lane)
            xt[static_cast<size_t>(x + lane)] = static_cast<uint64_t>(static_cast<int64_t>(va[lane]))
                + static_cast<uint64_t>(static_cast<int64_t>(vb[lane]));
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
