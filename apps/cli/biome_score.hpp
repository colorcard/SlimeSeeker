/* CLI 私有的 26.2 群系重评分与挂机点搜索。 */
#pragma once

#include "slimeseeker/slimeseeker.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ss::cli {

inline bool afk_chunk_center_in_range(int32_t chunk_x, int32_t chunk_z,
                                      double player_x, double player_z) {
    const double dx = static_cast<double>(chunk_x) * 16.0 + 8.0 - player_x;
    const double dz = static_cast<double>(chunk_z) * 16.0 + 8.0 - player_z;
    return dx * dx + dz * dz < 128.0 * 128.0;
}

inline bool afk_spawn_position_in_range(int64_t block_x, int32_t spawn_y, int64_t block_z,
                                        double player_x, int32_t player_y, double player_z) {
    const double dx = static_cast<double>(block_x) + 0.5 - player_x;
    const double dy = static_cast<double>(spawn_y - player_y);
    const double dz = static_cast<double>(block_z) + 0.5 - player_z;
    const double distance2 = dx * dx + dy * dy + dz * dz;
    return distance2 > 24.0 * 24.0 && distance2 <= 128.0 * 128.0;
}

struct BiomeRankedResult {
    ss_result source{};
    uint64_t biome_score_q32 = 0;
    double biome_score = 0.0;
    double common_equivalent_chunks = 0.0;
    double player_x = 0.0;
    int32_t player_y = -38;
    double player_z = 0.0;
    uint64_t afk_score_q32 = 0;
    double afk_score = 0.0;
};

class BiomeScorer {
public:
    BiomeScorer(int64_t seed, size_t top_count, int32_t spawn_y, int32_t player_y);
    ~BiomeScorer();
    BiomeScorer(BiomeScorer &&) noexcept;
    BiomeScorer &operator=(BiomeScorer &&) noexcept;
    BiomeScorer(const BiomeScorer &) = delete;
    BiomeScorer &operator=(const BiomeScorer &) = delete;

    void consider(const ss_result &result);
    std::vector<BiomeRankedResult> finish(const std::atomic<bool> *cancel = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ss::cli
