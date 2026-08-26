/*
 * 完整搜索后端与 CPU 位图算子内部接口。
 *
 * SearchBackend 统一 CPU/GPU 的完整搜索生命周期；BuildMapFn 仅供 CPU 搜索内部选择
 * scalar/AVX2/NEON 位图算子，GPU 不需要模拟同步主机指针写入。
 */
#pragma once

#include <cstdint>
#include "slimeseeker/slimeseeker.h"

namespace ss {

enum class SearchBackendKind { cpu, cuda, metal, vulkan };
using BackendAvailableFn = bool (*)();
using SearchFn = ss_status (*)(const ss_search_params_v1 &, const ss_search_options_v1 &,
                               const ss_callbacks_v1 &);

struct SearchBackend {
    SearchBackendKind kind;
    ss_backend public_kind;
    BackendAvailableFn available;
    SearchFn search;
};

const SearchBackend *select_search_backend(ss_backend requested, ss_backend &selected);

// xterm_scratch 至少包含 width 个元素，由 worker 持有并跨 tile 复用。
using BuildMapFn = void (*)(int64_t, int32_t, int32_t, int, int, uint8_t *, uint64_t *);

void build_map_scalar(int64_t, int32_t, int32_t, int, int, uint8_t *, uint64_t *);
void build_map_avx2(int64_t, int32_t, int32_t, int, int, uint8_t *, uint64_t *);
void build_map_neon(int64_t, int32_t, int32_t, int, int, uint8_t *, uint64_t *);
bool cpu_has_avx2();
bool cpu_has_neon();
BuildMapFn select_map_backend(ss_backend requested, ss_backend &selected);
bool cuda_available();
ss_status search_cuda(const ss_search_params_v1 &, const ss_search_options_v1 &, const ss_callbacks_v1 &);

} // namespace ss
