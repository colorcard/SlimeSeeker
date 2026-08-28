/*
 * SlimeSeeker 可执行程序入口。
 *
 * 无参数时进入交互式 TUI；任何传统参数继续交给命令行实现，保证脚本接口稳定。
 */
#include "cli.hpp"
#include "tui.hpp"

#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <fcntl.h>
#include <io.h>

namespace {
void configure_windows_utf8_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}
} // namespace
#endif

int main(int argc, char **argv) {
    const bool tui = argc == 1 || (argc == 2 && std::strcmp(argv[1], "--tui") == 0);
#if defined(_WIN32)
    if (tui) configure_windows_utf8_console();
#endif
    if (tui) return ss::cli::run_tui();
    return ss::cli::run_command_line(argc, argv);
}
