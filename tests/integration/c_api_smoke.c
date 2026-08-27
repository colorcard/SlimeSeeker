/*
 * 纯 C 调用集成测试入口。
 *
 * 由 C 编译器编译，用于防止公共头文件意外依赖 C++ 语法，并验证 C ABI 可链接运行。
 */
#include <slimeseeker/slimeseeker.h>
#include <string.h>

int main(void) {
    ss_search_params_v1 params = {
        sizeof(ss_search_params_v1), 0, -1, 1, -1, 1, 192, 0
    };
    ss_search_options_v1 options = {
        sizeof(ss_search_options_v1), 1, SS_BACKEND_SCALAR, 16
    };
    if (SS_API_VERSION != 1u || strcmp(ss_version(), "1.2.0") != 0) return 1;
    if (!ss_backend_available(SS_BACKEND_SCALAR)) return 2;
    return ss_search(&params, &options, 0) == SS_OK ? 0 : 3;
}
