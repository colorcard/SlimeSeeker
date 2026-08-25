/*
 * Minecraft Java Edition 史莱姆区块领域规则实现。
 *
 * 本文件维护精确 Java LCG、nextInt rejection sampling 和圆环段表，不依赖线程、
 * 命令行或具体指令集，是所有搜索后端共享的正确性基线。
 */
#include "core/domain.hpp"
#include <stdexcept>

namespace ss {

bool is_slime_from_chunk_seed(int64_t chunk_seed_value) {
    uint64_t state = (static_cast<uint64_t>(chunk_seed_value) ^ kLcgMul) & kLcgMask;
    for (;;) {
        // 精确复刻 java.util.Random.nextInt(10)。尾部 8 个值必须拒绝重抽，
        // 不能简化成一次 bits % 10，否则极低概率下会与游戏结果不一致。
        state = (state * kLcgMul + kLcgAdd) & kLcgMask;
        const uint32_t bits = static_cast<uint32_t>(state >> 17);
        const uint32_t value = bits % 10u;
        if (bits - value + 9u < 0x80000000u) return value == 0;
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
