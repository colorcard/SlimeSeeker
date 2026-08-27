#include "biome_score.hpp"

#include "core/domain.hpp"
#include "worldgen/worldgen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <queue>
#include <unordered_map>
#include <utility>

namespace ss::cli {
namespace {


int64_t result_distance(const ss_result &result) {
    return static_cast<int64_t>(result.x) * result.x + static_cast<int64_t>(result.z) * result.z;
}

bool better(const BiomeRankedResult &a, const BiomeRankedResult &b) {
    if (a.biome_score_q32 != b.biome_score_q32) return a.biome_score_q32 > b.biome_score_q32;
    if (a.source.count != b.source.count) return a.source.count > b.source.count;
    const int64_t ad = result_distance(a.source), bd = result_distance(b.source);
    if (ad != bd) return ad < bd;
    if (a.source.x != b.source.x) return a.source.x < b.source.x;
    return a.source.z < b.source.z;
}

struct BetterHeap {
    bool operator()(const BiomeRankedResult &a, const BiomeRankedResult &b) const { return better(a, b); }
};

uint64_t chunk_key(int32_t x, int32_t z) {
    return static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 | static_cast<uint32_t>(z);
}

} // namespace

struct BiomeScorer::Impl {
    struct ChunkCells {
        int32_t x;
        int32_t z;
        std::array<uint32_t, 256> ratio;
    };

    Impl(int64_t world_seed, size_t keep, int32_t spawn, int32_t player, worldgen::MinecraftVersion version)
        : seed(world_seed), top_count(keep), spawn_y(spawn), player_y(player), version(version), world(world_seed, version) {}

    uint64_t chunk_score(int32_t chunk_x, int32_t chunk_z) {
        const uint64_t key = chunk_key(chunk_x, chunk_z);
        if (const auto found = chunk_scores.find(key); found != chunk_scores.end()) return found->second;
        uint64_t score = 0;
        const int32_t base_x = chunk_x * 16;
        const int32_t base_z = chunk_z * 16;
        for (int32_t z = 0; z < 16; ++z)
            for (int32_t x = 0; x < 16; ++x)
                score += worldgen::spawn_ratio_q32(version, world.biome_at_block(base_x + x, spawn_y, base_z + z));

        constexpr size_t kCacheCapacity = 65536;
        if (chunk_scores.size() == kCacheCapacity) {
            chunk_scores.erase(cache_order.front());
            cache_order.pop_front();
        }
        chunk_scores.emplace(key, score);
        cache_order.push_back(key);
        return score;
    }

    std::vector<std::pair<int32_t, int32_t>> slime_chunks(const ss_result &result) const {
        std::vector<std::pair<int32_t, int32_t>> chunks;
        chunks.reserve(result.count);
        for (int dz = -kRadius; dz <= kRadius; ++dz) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                if (in_donut(dx, dz) && is_slime(seed, result.x + dx, result.z + dz))
                    chunks.emplace_back(result.x + dx, result.z + dz);
            }
        }
        return chunks;
    }

    bool load_cells(const std::vector<std::pair<int32_t, int32_t>> &chunks,
                    std::vector<ChunkCells> &result, const std::atomic<bool> *cancel) {
        result.reserve(chunks.size());
        for (const auto &[chunk_x, chunk_z] : chunks) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return false;
            ChunkCells cells{chunk_x, chunk_z, {}};
            const int32_t base_x = chunk_x * 16;
            const int32_t base_z = chunk_z * 16;
            for (int32_t z = 0; z < 16; ++z)
                for (int32_t x = 0; x < 16; ++x)
                    cells.ratio[static_cast<size_t>(z * 16 + x)] =
                        worldgen::spawn_ratio_q32(version, world.biome_at_block(base_x + x, spawn_y, base_z + z));
            result.push_back(std::move(cells));
        }
        return true;
    }

    uint64_t afk_score(const std::vector<ChunkCells> &chunks,
                     int32_t player_block_x, int32_t player_block_z) {
        uint64_t score = 0;
        const double player_x = player_block_x + 0.5;
        const double player_z = player_block_z + 0.5;
        for (const auto &chunk : chunks) {
            if (!afk_chunk_center_in_range(chunk.x, chunk.z, player_x, player_z)) continue;
            const int32_t base_x = chunk.x * 16;
            const int32_t base_z = chunk.z * 16;
            for (int32_t z = 0; z < 16; ++z) {
                for (int32_t x = 0; x < 16; ++x) {
                    if (afk_spawn_position_in_range(base_x + x, spawn_y, base_z + z,
                                                    player_x, player_y, player_z))
                        score += chunk.ratio[static_cast<size_t>(z * 16 + x)];
                }
            }
        }
        return score;
    }

    bool locate_player(BiomeRankedResult &result, const std::atomic<bool> *cancel) {
        std::vector<ChunkCells> chunks;
        if (!load_cells(slime_chunks(result.source), chunks, cancel)) return false;
        const int32_t base_x = result.source.x * 16;
        const int32_t base_z = result.source.z * 16;
        uint64_t best = 0;
        bool found = false;
        int32_t best_x = base_x, best_z = base_z;
        for (int32_t z = 0; z < 16; ++z) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return false;
            for (int32_t x = 0; x < 16; ++x) {
                const uint64_t score = afk_score(chunks, base_x + x, base_z + z);
                if (!found || score > best) {
                    found = true;
                    best = score;
                    best_x = base_x + x;
                    best_z = base_z + z;
                }
            }
        }
        result.player_x = best_x + 0.5;
        result.player_y = player_y;
        result.player_z = best_z + 0.5;
        result.afk_score_q32 = best;
        result.afk_score = static_cast<double>(best) / (4294967296.0 * 256.0);
        return true;
    }

    int64_t seed;
    size_t top_count;
    int32_t spawn_y;
    int32_t player_y;
    worldgen::Worldgen world;
    worldgen::MinecraftVersion version;
    std::unordered_map<uint64_t, uint64_t> chunk_scores;
    std::deque<uint64_t> cache_order;
    std::priority_queue<BiomeRankedResult, std::vector<BiomeRankedResult>, BetterHeap> top;
};

BiomeScorer::BiomeScorer(int64_t seed, size_t top_count, int32_t spawn_y, int32_t player_y,
                         worldgen::MinecraftVersion version)
    : impl_(std::make_unique<Impl>(seed, top_count, spawn_y, player_y, version)) {}
BiomeScorer::~BiomeScorer() = default;
BiomeScorer::BiomeScorer(BiomeScorer &&) noexcept = default;
BiomeScorer &BiomeScorer::operator=(BiomeScorer &&) noexcept = default;

void BiomeScorer::consider(const ss_result &source) {
    uint64_t total = 0;
    for (int dz = -kRadius; dz <= kRadius; ++dz)
        for (int dx = -kRadius; dx <= kRadius; ++dx)
            if (in_donut(dx, dz) && is_slime(impl_->seed, source.x + dx, source.z + dz))
                total += impl_->chunk_score(source.x + dx, source.z + dz);

    BiomeRankedResult result;
    result.source = source;
    result.biome_score_q32 = total;
    result.biome_score = static_cast<double>(total) / (4294967296.0 * 256.0);
    result.common_equivalent_chunks = result.biome_score / (400.0 / 515.0);
    if (impl_->top.size() < impl_->top_count) impl_->top.push(result);
    else if (better(result, impl_->top.top())) {
        impl_->top.pop();
        impl_->top.push(result);
    }
}

std::vector<BiomeRankedResult> BiomeScorer::finish(const std::atomic<bool> *cancel,
                                                    std::atomic<uint64_t> *completed_count) {
    std::vector<BiomeRankedResult> results;
    results.reserve(impl_->top.size());
    while (!impl_->top.empty()) {
        results.push_back(impl_->top.top());
        impl_->top.pop();
    }
    std::sort(results.begin(), results.end(), better);
    if (completed_count) completed_count->store(0, std::memory_order_relaxed);
    size_t completed_results = 0;
    for (auto &result : results) {
        if (!impl_->locate_player(result, cancel)) break;
        ++completed_results;
        if (completed_count)
            completed_count->store(completed_results, std::memory_order_relaxed);
    }
    results.resize(completed_results);
    return results;
}

} // namespace ss::cli
