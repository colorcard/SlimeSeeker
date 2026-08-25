#include "internal.hpp"
#include <cstddef>
#include <new>

extern "C" {

const char *ss_version(void) { return "1.0.0"; }

const char *ss_status_string(ss_status status) {
    switch (status) {
        case SS_OK: return "success";
        case SS_INVALID_ARGUMENT: return "invalid argument";
        case SS_CANCELLED: return "cancelled";
        case SS_CALLBACK_ABORTED: return "callback aborted";
        case SS_OUT_OF_MEMORY: return "out of memory";
        case SS_BACKEND_UNAVAILABLE: return "backend unavailable";
        case SS_INTERNAL_ERROR: return "internal error";
    }
    return "unknown status";
}

const char *ss_backend_name(ss_backend backend) {
    switch (backend) {
        case SS_BACKEND_AUTO: return "auto";
        case SS_BACKEND_SCALAR: return "scalar";
        case SS_BACKEND_AVX2: return "avx2";
        case SS_BACKEND_NEON: return "neon";
    }
    return "unknown";
}

int ss_backend_available(ss_backend backend) {
    switch (backend) {
        case SS_BACKEND_AUTO:
        case SS_BACKEND_SCALAR: return 1;
        case SS_BACKEND_AVX2: return ss::cpu_has_avx2() ? 1 : 0;
        case SS_BACKEND_NEON: return ss::cpu_has_neon() ? 1 : 0;
    }
    return 0;
}

ss_status ss_search(const ss_search_params_v1 *params,
                    const ss_search_options_v1 *options,
                    const ss_callbacks_v1 *callbacks) {
    // struct_size 为 ABI 向后兼容预留：旧调用方传入更短结构时不会越界读取。
    if (!params || params->struct_size < sizeof(ss_search_params_v1)) return SS_INVALID_ARGUMENT;
    if (params->threshold > SS_DONUT_CELLS || params->x_end < params->x_begin || params->z_end < params->z_begin)
        return SS_INVALID_ARGUMENT;
    constexpr int32_t low = INT32_MIN + ss::kRadius;
    constexpr int32_t high_exclusive = INT32_MAX - ss::kRadius + 1;
    // 搜索引擎会为中心坐标外扩 8 格；在入口拒绝可能溢出 int32_t 的边界。
    if (params->x_begin < low || params->z_begin < low ||
        params->x_end > high_exclusive || params->z_end > high_exclusive) return SS_INVALID_ARGUMENT;

    ss_search_options_v1 effective_options{sizeof(ss_search_options_v1), 0, SS_BACKEND_AUTO, 1024};
    if (options) {
        if (options->struct_size < sizeof(ss_search_options_v1) || options->result_batch_capacity > (1u << 24))
            return SS_INVALID_ARGUMENT;
        effective_options = *options;
    }
    ss_callbacks_v1 effective_callbacks{sizeof(ss_callbacks_v1), nullptr, nullptr, nullptr, nullptr};
    if (callbacks) {
        if (callbacks->struct_size < sizeof(ss_callbacks_v1)) return SS_INVALID_ARGUMENT;
        effective_callbacks = *callbacks;
    }
    try {
        return ss::search_impl(*params, effective_options, effective_callbacks);
    // C ABI 绝不允许异常穿越边界，统一转换成稳定状态码。
    } catch (const std::bad_alloc &) {
        return SS_OUT_OF_MEMORY;
    } catch (...) {
        return SS_INTERNAL_ERROR;
    }
}

} // extern C
