# SlimeSeeker

SlimeSeeker 用固定世界种子寻找史莱姆农场的候选位置。它先在指定的区块范围内搜索史莱姆区块密集的中心，再按指定 Minecraft 版本的群系刷怪表重新评分，并在每个候选中心的 16×16 方块内寻找更合适的挂机点。

它面向两类场景：普通史莱姆农场的快速选址，以及空置域中通过地狱门切门集中怪物容量的精细选址。工具比较的是区块密度、群系权重和挂机圆环覆盖，不模拟地形、结构、地狱门布局、运输、击杀延迟或最终 items/hour。

## 从哪里开始

### TUI

在交互式终端直接运行：

```sh
./build/slimeseeker
```

配置页中选择搜索模式。区块密度模式只使用史莱姆区块数量排序，群系评分模式会继续计算群系权重和挂机覆盖。群系模式提供 Minecraft 1.18.2、1.19.4、1.20.6、1.21.3 和 26.2 五个版本，默认选中 26.2；版本选择会显示在配置摘要和运行阶段中。

建议先用较小范围确认结果规模，再逐步扩大范围。`Range = R` 搜索的是 `[-R, R) × [-R, R)` 的中心区块。阈值越高，结果越少，但每个结果的史莱姆区块密度越高。空置域切门农场应选择与存档一致的 Minecraft 版本，填写生成层脚部 Y 和玩家脚部 Y，然后比较群系分、普通群系等效区块和挂机分。

运行页分开显示区块搜索、群系评分和挂机点扫描。结果页提供候选表和 17×17 可视化：圆环中的史莱姆区块、挂机点、有效生成范围以及边界区块都会分别标出。结果可以导出为 CSV；取消后的结果会标记为部分结果。

主要操作：`Tab` / `Shift-Tab` 移动焦点，方向键切换选项，`Enter` 执行按钮，`Esc` 返回或关闭对话框，`F5` 开始搜索。

### CLI

普通搜索：

```text
slimeseeker [OPTIONS] SEED RANGE [THRESHOLD]
```

群系评分：

```text
slimeseeker biome-score [OPTIONS] SEED RANGE [THRESHOLD]
```

常用示例：

```sh
# 搜索 seed 0，中心范围 [-10000,10000)，阈值 45
./build/slimeseeker 0 10000 45

# 输出 CSV，只保留前 20 个中心
./build/slimeseeker --top 20 -f csv 12345 10000 45

# 使用 1.20.6 群系数据进行评分
./build/slimeseeker biome-score --mc-version 1.20.6 --top 20 -f csv 12345 10000 45
```

| 选项 | 说明 |
|---|---|
| `-j, --threads N` | worker 数量，`0` 自动选择 |
| `-m, --backend auto\|scalar\|avx2\|neon\|cuda` | 选择计算后端 |
| `-f, --format human\|csv` | 输出格式 |
| `--top K` | 只保留排序最优的 K 条结果 |
| `--unordered` | 按到达顺序流式输出，不能用于 `biome-score` |
| `--mc-version VERSION` | 群系版本：`1.18.2`、`1.19.4`、`1.20.6`、`1.21.3`、`26.2` |
| `--spawn-y Y` | 群系采样和生成脚部高度，默认 `-63` |
| `--player-y Y` | 挂机玩家脚部高度，默认 `-38` |
| `-q, --quiet` | 关闭进度显示 |
| `-b, --benchmark` | 只报告吞吐，不能用于 `biome-score` |

## 游戏内判定模型

### 史莱姆区块

每个区块使用 Minecraft Java Edition 的区块种子算法判定史莱姆区块。实现保持 Java `int` 的 32 位补码回绕、48 位 LCG 和 `Random.nextInt(10)` 的 rejection sampling，因此不会用浮点近似或有偏取模替代原版随机过程。

### 生成圆环

每个中心的固定搜索圆环满足：

```text
1 < dx² + dz² <= 64
```

圆环位于 17×17 区块窗口内，共 192 个区块。第一阶段返回圆环内史莱姆区块数量达到阈值的中心，结果按数量降序、距离原点升序、X、Z 排序。

### 群系评分

第二阶段只处理第一阶段命中的中心。对每个史莱姆区块的 256 个方块，在指定生成高度采样所选 Minecraft 版本的主世界群系。每个方块的权重为：

```text
slime_group_weight / monster_weight
```

`monster_weight` 是该群系 MONSTER 列表的总权重，`slime_group_weight` 是史莱姆条目权重乘以固定群组数后的贡献。256 个方块的平均值形成一个区块的修正分，所有史莱姆区块的修正分相加得到 `biome_score`。`common_equivalent_chunks` 将它换算成普通群系下的等效区块数，便于和第一阶段的 `count` 对照。

当前支持的版本和群系采样器如下：

| 版本 | 气候采样 | 参数分区 | 说明 |
|---|---|---|---|
| 1.18.2 | 版本化 Cubiomes sampler | 兼容基线 | 可替换为独立的 `OverworldBiomeBuilder` 数据 |
| 1.19.4 | 版本化 Cubiomes sampler | 兼容基线 | 同上 |
| 1.20.6 | 版本化 Cubiomes sampler | 兼容基线 | 同上 |
| 1.21.3 | 版本化 Cubiomes sampler | 兼容基线 | 同上 |
| 26.2 | 版本化 Cubiomes sampler | 26.2 数据文件 | 包含硫磺洞穴权重档 |

1.18.2 至 1.21.3 当前使用同一套参数分区作为兼容基线，但气候噪声已经按版本切换。26.2 专属的硫磺洞穴档位在旧版本中按零权重处理。参数数据位于 `src/worldgen/generated/`，后续可以逐版本替换，不需要改动评分和搜索流程。

### 挂机点

群系 Top-K 产生后，工具在每个候选中心区块的 16×16 方块中心搜索挂机 X/Z。一个方块计入挂机分需要同时满足区块中心水平距离小于 128，以及生成脚部到玩家脚部的三维距离 `24² < d² <= 128²`。挂机点只作为候选的附加指标，不会再次改变群系排名。

## 输出

普通 CSV：

```text
x,z,count
```

群系 CSV：

```text
rank,x,z,count,biome_score,common_equivalent_chunks,player_x,player_y,player_z,afk_score
```

`biome_score` 和 `afk_score` 以普通群系等效区块为便于比较的单位。它们用于候选排序和挂机点比较，不等同于实测产量。

## 构建

需要 CMake 3.24 以上版本和支持 C++20 的 MSVC、GCC 或 Clang。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

默认会构建库、CLI、TUI、测试和基准程序。只构建库与传统 CLI：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSLIMESEEKER_BUILD_TUI=OFF \
  -DSLIMESEEKER_BUILD_TESTS=OFF \
  -DSLIMESEEKER_BUILD_BENCHMARKS=OFF
cmake --build build --parallel
```

TUI 默认使用 FTXUI 7.0.3。已有系统 FTXUI 且需要离线配置时设置 `-DSLIMESEEKER_FTXUI_PROVIDER=system`。CUDA 构建使用 `-DSLIMESEEKER_ENABLE_CUDA=ON`；`--backend cuda` 只在运行时显式启用 CUDA。

## 开发者说明

公共接口位于 `include/slimeseeker/slimeseeker.h`，保持纯 C ABI，并通过 `struct_size` 支持结构版本演进。内部依赖方向为 `apps/api → engine → core + backends`；多版本群系模块位于 `src/worldgen/`，通过版本枚举选择 Cubiomes sampler 和参数数据，不向公共 ABI 暴露具体版本类型。

CPU 后端包含 scalar、AVX2 和 NEON，运行时检查能力；scalar 是逐位正确性基线。CUDA 使用独立后端。搜索区域采用 tile 动态调度、worker 独占 scratch 和有界结果批次，避免热路径共享锁以及结果无限积压。

基准程序：

```sh
./build/slimeseeker_bench 5000 1 45
```

参数依次为 `[range] [threads] [threshold]`。性能比较应固定机器、编译器、后端和输入，并分别记录冷启动与热运行。

## C ABI 示例

```c
#include <slimeseeker/slimeseeker.h>

static int receive_results(void *context, const ss_result *results, size_t count) {
    size_t *total = (size_t *)context;
    *total += count;
    return 0;
}

int main(void) {
    ss_search_params_v1 params = {sizeof(params), 0, -1000, 1000, -1000, 1000, 45, 0};
    ss_search_options_v1 options = {sizeof(options), 0, SS_BACKEND_AUTO, 1024};
    size_t total = 0;
    ss_callbacks_v1 callbacks = {sizeof(callbacks), &total, receive_results, NULL, NULL};
    return (int)ss_search(&params, &options, &callbacks);
}
```

安装后的 CMake 消费方：

```cmake
find_package(SlimeSeeker CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE SlimeSeeker::slimeseeker)
```

## 参考

群系参数和刷怪表来自 Minecraft Java Edition 的版本化源码与数据文件，Cubiomes 用于对应版本的气候噪声采样。`refer/SlimeRadar` 仅用于人工核对历史结果，不参与构建或运行。
