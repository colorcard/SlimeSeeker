/* Minecraft 26.2 默认主世界群系采样器，仅供 CLI 第二阶段使用。 */
#pragma once

#include <cstdint>
#include <memory>

namespace ss::worldgen26 {

enum class BiomeProfile : uint8_t {
    common,
    old_growth_pine,
    jungle,
    ocean,
    frozen_river,
    river,
    dripstone,
    swamp,
    sulfur,
    empty,
};

struct SpawnWeight {
    uint16_t slime_group_weight;
    uint16_t monster_weight;
};

SpawnWeight spawn_weight(BiomeProfile profile);
double spawn_ratio(BiomeProfile profile);
uint32_t spawn_ratio_q32(BiomeProfile profile);

class Worldgen26 {
public:
    explicit Worldgen26(int64_t seed);
    ~Worldgen26();
    Worldgen26(Worldgen26 &&) noexcept;
    Worldgen26 &operator=(Worldgen26 &&) noexcept;
    Worldgen26(const Worldgen26 &) = delete;
    Worldgen26 &operator=(const Worldgen26 &) = delete;

    BiomeProfile biome_at_block(int32_t x, int32_t y, int32_t z);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ss::worldgen26
