#include "internal.hpp"

namespace ss {

void build_map_scalar(int64_t seed, int32_t x0, int32_t z0, int width, int height, uint8_t *out) {
    // x 项跨所有 z 行复用；每行只需计算一次 zbase，逐格热路径剩一次加法和异或。
    std::vector<uint64_t> xt(static_cast<size_t>(width));
    for (int x = 0; x < width; ++x) xt[static_cast<size_t>(x)] = xterm(x0 + x);
    for (int z = 0; z < height; ++z) {
        const uint64_t zb = zbase(seed, z0 + z);
        auto *row = out + static_cast<size_t>(z) * static_cast<size_t>(width);
        for (int x = 0; x < width; ++x) {
            const auto cs = static_cast<int64_t>((zb + xt[static_cast<size_t>(x)]) ^ kChunkXor);
            row[x] = static_cast<uint8_t>(is_slime_from_chunk_seed(cs));
        }
    }
}

} // namespace ss
