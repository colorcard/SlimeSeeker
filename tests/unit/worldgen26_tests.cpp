#include "worldgen26/worldgen26.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void expect_profile(ss::worldgen26::Worldgen26 &world, int x, int y, int z,
                    ss::worldgen26::BiomeProfile expected) {
    if (world.biome_at_block(x, y, z) != expected) {
        std::fprintf(stderr, "unexpected Minecraft 26.2 biome profile at (%d,%d,%d)\n", x, y, z);
        std::exit(1);
    }
}

} // namespace

int main() {
    using ss::worldgen26::BiomeProfile;
    ss::worldgen26::Worldgen26 seed_zero(0);
    expect_profile(seed_zero, 0, -63, 0, BiomeProfile::river);
    expect_profile(seed_zero, 224, -63, 320, BiomeProfile::common);
    expect_profile(seed_zero, -1024, -63, 2048, BiomeProfile::empty);
    expect_profile(seed_zero, 12345, -63, -6789, BiomeProfile::common);
    expect_profile(seed_zero, -30000000, -63, 30000000, BiomeProfile::common);
    // Coordinates were sampled directly from the unmodified 26.2 BiomeManager.
    expect_profile(seed_zero, -6304, -63, -20000, BiomeProfile::jungle);
    expect_profile(seed_zero, -18720, -63, -20000, BiomeProfile::ocean);
    expect_profile(seed_zero, -2464, -63, -19872, BiomeProfile::dripstone);
    expect_profile(seed_zero, -3616, -63, -20000, BiomeProfile::frozen_river);
    expect_profile(seed_zero, -6688, -63, -20000, BiomeProfile::swamp);
    expect_profile(seed_zero, 13280, -63, -19488, BiomeProfile::empty);
    expect_profile(seed_zero, 11872, -63, -19744, BiomeProfile::old_growth_pine);
    expect_profile(seed_zero, -17312, -63, -20000, BiomeProfile::river);
    expect_profile(seed_zero, 17248, -63, -17056, BiomeProfile::sulfur);
    expect_profile(seed_zero, -10784, -63, -19488, BiomeProfile::swamp);

    const auto swamp = ss::worldgen26::spawn_weight(BiomeProfile::swamp);
    if (swamp.slime_group_weight != 401 || swamp.monster_weight != 516) return 1;
    const auto sulfur = ss::worldgen26::spawn_weight(BiomeProfile::sulfur);
    if (sulfur.slime_group_weight != 25 || sulfur.monster_weight != 311) return 1;
    const auto old_sulfur = ss::worldgen26::spawn_weight(
        ss::worldgen26::MinecraftVersion::v1_20_6, BiomeProfile::sulfur);
    if (old_sulfur.slime_group_weight != 0 || old_sulfur.monster_weight != 0) return 1;
    ss::worldgen26::MinecraftVersion parsed{};
    if (!ss::worldgen26::parse_version("1.18", parsed) ||
        parsed != ss::worldgen26::MinecraftVersion::v1_18_2 ||
        std::string(ss::worldgen26::version_name(parsed)) != "1.18.2") return 1;
    return 0;
}
