/*
 * 搜索引擎内部入口。
 *
 * C ABI 层在完成参数规范化后调用统一分派入口；CPU 实现保持独立，以便完整搜索后端
 * 与 CPU 内部位图算子分别演进。
 */
#pragma once

#include "slimeseeker/slimeseeker.h"

namespace ss {

ss_status search_impl(const ss_search_params_v1 &params,
                      const ss_search_options_v1 &options,
                      const ss_callbacks_v1 &callbacks);
ss_status search_cpu_impl(const ss_search_params_v1 &params,
                          const ss_search_options_v1 &options,
                          const ss_callbacks_v1 &callbacks);

} // namespace ss
