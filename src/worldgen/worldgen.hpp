/* Minecraft 主世界多版本群系采样器，仅供 CLI 第二阶段使用。 */
#pragma once

#include <cstdint>
#include <memory>

namespace ss::worldgen {

enum class MinecraftVersion : uint8_t {
    v1_18_2,
    v1_19_4,
    v1_20_6,
    v1_21_3,
    v26_2,
};

MinecraftVersion default_version();
const char *version_name(MinecraftVersion version);
bool parse_version(const char *text, MinecraftVersion &version);

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
SpawnWeight spawn_weight(MinecraftVersion version, BiomeProfile profile);
double spawn_ratio(BiomeProfile profile);
double spawn_ratio(MinecraftVersion version, BiomeProfile profile);
uint32_t spawn_ratio_q32(BiomeProfile profile);
uint32_t spawn_ratio_q32(MinecraftVersion version, BiomeProfile profile);

class Worldgen {
public:
    explicit Worldgen(int64_t seed, MinecraftVersion version = default_version());
    ~Worldgen();
    Worldgen(Worldgen &&) noexcept;
    Worldgen &operator=(Worldgen &&) noexcept;
    Worldgen(const Worldgen &) = delete;
    Worldgen &operator=(const Worldgen &) = delete;

    BiomeProfile biome_at_block(int32_t x, int32_t y, int32_t z);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ss::worldgen
