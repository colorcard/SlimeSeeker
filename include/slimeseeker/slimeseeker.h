/*
 * SlimeSeeker 唯一公共 C ABI 头文件。
 *
 * API 同时兼容 C 与 C++；所有结构通过 struct_size 支持版本演进，调用方不得依赖
 * 内部 C++ 类型、线程实现或后端布局。
 */
#ifndef SLIMESEEKER_SLIMESEEKER_H
#define SLIMESEEKER_SLIMESEEKER_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(SLIMESEEKER_SHARED)
# if defined(SLIMESEEKER_BUILDING_LIBRARY)
#  define SS_API __declspec(dllexport)
# else
#  define SS_API __declspec(dllimport)
# endif
#else
# define SS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SS_API_VERSION 1u
#define SS_DONUT_CELLS 192u

typedef enum ss_status {
    SS_OK = 0,
    SS_INVALID_ARGUMENT = 1,
    SS_CANCELLED = 2,
    SS_CALLBACK_ABORTED = 3,
    SS_OUT_OF_MEMORY = 4,
    SS_BACKEND_UNAVAILABLE = 5,
    SS_INTERNAL_ERROR = 6
} ss_status;

typedef enum ss_backend {
    SS_BACKEND_AUTO = 0,
    SS_BACKEND_SCALAR = 1,
    SS_BACKEND_AVX2 = 2,
    SS_BACKEND_NEON = 3,
    SS_BACKEND_CUDA = 4
} ss_backend;

/* 搜索中心采用半开区间 [begin,end)；实现会在四周额外读取 8 个区块。 */
typedef struct ss_search_params_v1 {
    uint32_t struct_size;
    int64_t world_seed;
    int32_t x_begin;
    int32_t x_end;
    int32_t z_begin;
    int32_t z_end;
    uint16_t threshold;
    uint16_t reserved;
} ss_search_params_v1;

typedef struct ss_search_options_v1 {
    uint32_t struct_size;
    uint32_t threads;
    ss_backend backend;
    uint32_t result_batch_capacity;
} ss_search_options_v1;

typedef struct ss_result {
    int32_t x;
    int32_t z;
    uint16_t count;
    uint16_t reserved;
} ss_result;

/* on_results 返回非零会以 SS_CALLBACK_ABORTED 终止。
 * 三类回调均由调用 ss_search 的线程串行触发，无需调用方额外加锁。 */
typedef int (*ss_results_fn)(void *context, const ss_result *results, size_t count);
typedef void (*ss_progress_fn)(void *context, uint64_t completed, uint64_t total);
typedef int (*ss_cancel_fn)(void *context);

typedef struct ss_callbacks_v1 {
    uint32_t struct_size;
    void *context;
    ss_results_fn on_results;
    ss_progress_fn on_progress;
    ss_cancel_fn should_cancel;
} ss_callbacks_v1;

SS_API const char *ss_version(void);
SS_API const char *ss_status_string(ss_status status);
SS_API const char *ss_backend_name(ss_backend backend);
SS_API int ss_backend_available(ss_backend backend);
/* options/callbacks 可传 NULL 使用默认值；所有传入结构必须正确填写 struct_size。 */
SS_API ss_status ss_search(const ss_search_params_v1 *params,
                           const ss_search_options_v1 *options,
                           const ss_callbacks_v1 *callbacks);

#ifdef __cplusplus
}
#endif
#endif
