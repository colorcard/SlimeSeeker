/*
 * 完整搜索后端统一分派。
 *
 * CPU 搜索与 CUDA 搜索通过同一描述符契约注册；auto/scalar/AVX2/NEON 都选择 CPU
 * 搜索生命周期，具体 CPU 位图算子仍由 CPU 实现内部校准和选择。
 */
#include "backends/backend.hpp"
#include "engine/search.hpp"

namespace ss {
namespace {

bool cpu_available() { return true; }

const SearchBackend kCpuBackend{
    SearchBackendKind::cpu, SS_BACKEND_AUTO, cpu_available, search_cpu_impl};
const SearchBackend kCudaBackend{
    SearchBackendKind::cuda, SS_BACKEND_CUDA, cuda_available, search_cuda};

} // namespace

const SearchBackend *select_search_backend(ss_backend requested, ss_backend &selected) {
    switch (requested) {
        case SS_BACKEND_AUTO:
        case SS_BACKEND_SCALAR:
        case SS_BACKEND_AVX2:
        case SS_BACKEND_NEON:
            selected = requested;
            return &kCpuBackend;
        case SS_BACKEND_CUDA:
            selected = SS_BACKEND_CUDA;
            return &kCudaBackend;
    }
    return nullptr;
}

ss_status search_impl(const ss_search_params_v1 &params, const ss_search_options_v1 &options,
                      const ss_callbacks_v1 &callbacks) {
    ss_backend selected{};
    const SearchBackend *backend = select_search_backend(options.backend, selected);
    if (!backend || !backend->available()) return SS_BACKEND_UNAVAILABLE;
    return backend->search(params, options, callbacks);
}

} // namespace ss
