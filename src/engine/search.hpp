/*
 * 搜索引擎内部入口。
 *
 * C ABI 层在完成参数规范化后调用这里；实现负责整个 CPU 搜索生命周期，但不承担
 * 公共 ABI 校验，也不定义 Minecraft 领域公式。
 */
#pragma once

#include "slimeseeker/slimeseeker.h"

namespace ss {

ss_status search_impl(const ss_search_params_v1 &params,
                      const ss_search_options_v1 &options,
                      const ss_callbacks_v1 &callbacks);

} // namespace ss

