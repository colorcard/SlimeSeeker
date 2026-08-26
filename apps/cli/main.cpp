/*
 * SlimeSeeker 可执行程序入口。
 *
 * 无参数时进入交互式 TUI；任何传统参数继续交给命令行实现，保证脚本接口稳定。
 */
#include "cli.hpp"
#include "tui.hpp"

#include <cstring>

int main(int argc, char **argv) {
    if (argc == 1) return ss::cli::run_tui();
    if (argc == 2 && std::strcmp(argv[1], "--tui") == 0) return ss::cli::run_tui();
    return ss::cli::run_command_line(argc, argv);
}
