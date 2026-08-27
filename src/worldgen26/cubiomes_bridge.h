#ifndef SLIMESEEKER_CUBIOMES_BRIDGE_H
#define SLIMESEEKER_CUBIOMES_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ss_cubiomes_sampler ss_cubiomes_sampler;

ss_cubiomes_sampler *ss_cubiomes_create(int64_t seed);
void ss_cubiomes_destroy(ss_cubiomes_sampler *sampler);
void ss_cubiomes_climate(const ss_cubiomes_sampler *sampler, int block_x, int block_y, int block_z,
                         int64_t climate[6]);

#ifdef __cplusplus
}
#endif

#endif
