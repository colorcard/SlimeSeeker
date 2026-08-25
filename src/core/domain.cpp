/*
 * Minecraft Java Edition 史莱姆区块领域规则实现。
 *
 * 本文件维护精确 Java LCG、nextInt rejection sampling 和圆环段表，不依赖线程、
 * 命令行或具体指令集，是所有搜索后端共享的正确性基线。
 */
#include "core/domain.hpp"
#include <stdexcept>

namespace ss {

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
bool next_int10_retry(uint64_t rejected_state) {
    // 入口状态已经产生过一次被拒绝的 31 位值；从下一次 LCG 状态继续，不能重新播种。
    uint64_t state = rejected_state;
    for (;;) {
        state = (state * kLcgMul + kLcgAdd) & kLcgMask;
        const uint32_t bits = static_cast<uint32_t>(state >> 17);
        if (bits < kNextInt10Limit) return bits % 10u == 0;
    }
}

bool in_donut(int dx, int dz) {
    const int distance2 = dx * dx + dz * dz;
    return distance2 > 1 && distance2 <= 64;
}

const Runs &donut_runs() {
    // 从几何定义生成段表，而不是手写常量，避免掩码与优化查询逻辑漂移。
    static const Runs runs = [] {
        Runs result{};
        size_t index = 0;
        int cells = 0;
        for (int row = 0; row < kWindow; ++row) {
            bool active = false;
            int first = 0;
            for (int column = 0; column <= kWindow; ++column) {
                const bool inside = column < kWindow && in_donut(row - kRadius, column - kRadius);
                if (inside) ++cells;
                if (inside && !active) { active = true; first = column; }
                if (!inside && active) {
                    if (index >= result.size()) throw std::logic_error("invalid donut run count");
                    result[index++] = Run{static_cast<uint8_t>(row), static_cast<uint8_t>(first),
                                          static_cast<uint8_t>(column - 1)};
                    active = false;
                }
            }
        }
        if (index != result.size() || cells != static_cast<int>(SS_DONUT_CELLS))
            throw std::logic_error("invalid donut geometry");
        return result;
    }();
    return runs;
}

} // namespace ss
