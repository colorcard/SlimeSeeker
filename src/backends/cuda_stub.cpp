/* CUDA 未编译时的完整搜索后端占位实现。 */
#include "backends/backend.hpp"

namespace ss {

bool cuda_available() { return false; }
ss_status search_cuda(const ss_search_params_v1 &, const ss_search_options_v1 &,
                      const ss_callbacks_v1 &) {
    return SS_BACKEND_UNAVAILABLE;
}

} // namespace ss
