/* CLI 私有的 26.2 群系重评分与挂机点搜索。 */
#pragma once

#include "slimeseeker/slimeseeker.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ss::cli {

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
    std::vector<BiomeRankedResult> finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ss::cli
