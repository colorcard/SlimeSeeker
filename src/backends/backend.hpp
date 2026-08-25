/*
 * CPU 位图算子后端内部接口。
 *
 * 所有实现把同一矩形转换成逐字节史莱姆标记；dispatch 负责能力检测和自动校准，
 * 搜索引擎只依赖此统一函数签名，不直接引用 AVX2/NEON intrinsic。
 */
#pragma once

#include <cstdint>
#include "slimeseeker/slimeseeker.h"

namespace ss {

// xterm_scratch 至少包含 width 个元素，由 worker 持有并跨 tile 复用。
using BuildMapFn = void (*)(int64_t, int32_t, int32_t, int, int, uint8_t *, uint64_t *);

void build_map_scalar(int64_t, int32_t, int32_t, int, int, uint8_t *, uint64_t *);
void build_map_avx2(int64_t, int32_t, int32_t, int, int, uint8_t *, uint64_t *);
void build_map_neon(int64_t, int32_t, int32_t, int, int, uint8_t *, uint64_t *);
bool cpu_has_avx2();
bool cpu_has_neon();
BuildMapFn select_backend(ss_backend requested, ss_backend &selected);
bool cuda_available();
ss_status search_cuda(const ss_search_params_v1 &, const ss_search_options_v1 &, const ss_callbacks_v1 &);

} // namespace ss
