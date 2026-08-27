#include "worldgen/cubiomes_bridge.h"
#include "worldgen/third_party/cubiomes/biomenoise.h"

#include <stdlib.h>

struct ss_cubiomes_sampler {
    BiomeNoise noise;
    uint64_t sha;
};

ss_cubiomes_sampler *ss_cubiomes_create(int64_t seed, int minecraft_version) {
    ss_cubiomes_sampler *sampler = (ss_cubiomes_sampler *)calloc(1, sizeof(*sampler));
    if (sampler == NULL) return NULL;
    sampler->sha = getVoronoiSHA((uint64_t)seed);
    initBiomeNoise(&sampler->noise, minecraft_version);
    setBiomeSeed(&sampler->noise, (uint64_t)seed, 0);
    return sampler;
}

void ss_cubiomes_destroy(ss_cubiomes_sampler *sampler) {
    free(sampler);
}

void ss_cubiomes_climate(const ss_cubiomes_sampler *sampler, int block_x, int block_y, int block_z,
                         int64_t climate[6]) {
    int quart_x, quart_y, quart_z;
    voronoiAccess3D(sampler->sha, block_x, block_y, block_z, &quart_x, &quart_y, &quart_z);
    sampleBiomeNoise(&sampler->noise, climate, quart_x, quart_y, quart_z, NULL, SAMPLE_NO_BIOME);
}
