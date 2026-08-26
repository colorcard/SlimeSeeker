/* 未构建交互界面时提供稳定的 TUI 入口。 */
#include "tui.hpp"

#include <cstdio>

namespace ss::cli {
int run_tui() {
    std::fprintf(stderr,
        "interactive TUI is unavailable in this build; provide SEED and RANGE arguments\n");
    return 1;
}
}
