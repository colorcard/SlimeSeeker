# SlimeSeeker

SlimeSeeker 是一个跨平台的 Minecraft Java Edition 史莱姆区块密度搜索库与命令行程序。给定世界种子，它会在指定区块坐标范围内搜索 17×17 圆环中史莱姆区块数量达到阈值的中心点。

项目以结果精确、CPU 跨平台、专用算子可替换和公共 ABI 稳定为设计目标。内部使用 C++20，对外提供纯 C ABI；支持多线程搜索、运行时 AVX2/NEON 分派、CSV、流式输出、Top-K、进度与取消。搜索结果通过有界批次交付，不设置固定总容量，也不会以成功状态返回截断结果。

## 功能特性

- 精确复刻 Minecraft Java Edition 的区块种子运算、48 位 Java LCG 与 `Random.nextInt(10)` rejection sampling；
- 搜索固定几何 `1 < dx² + dz² <= 64`，17×17 窗口内共包含 192 个区块；
- 使用 tile 动态调度和 worker 独占 scratch，支持单线程与多线程运行；
- 提供完整 CPU 与可选 CUDA 搜索后端；CPU 内部支持独立编译和运行时检测的 scalar、AVX2、NEON 位图算子；
- 按阈值选择滑动圆环、二维 SAT 或滑动方框搜索管线；
- 提供稳定的版本化 C ABI，回调支持批量结果、进度、取消和主动终止；
- 默认提供中英文 TUI 工作台，支持参数配置、实时进度、取消、分页结果与后台 CSV 导出；
- 传统 CLI 支持确定性全量排序、无序流式输出、Top-K 保留、CSV 和基准模式；
- 包含朴素差分、后端一致性、并发契约、C ABI 和完整黄金回归测试。

## 构建

需要 CMake 3.24 或更高版本、支持 C++20 的 MSVC/GCC/Clang，以及操作系统线程库。

Release 构建与测试：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Unix 类平台的 CLI 通常位于 `build/slimeseeker`；Visual Studio 等多配置生成器通常位于 `build/Release/slimeseeker.exe`。默认同时构建库、CLI、测试和基准程序。

常用 CMake 选项：

| 选项 | 默认值 | 作用 |
|---|---:|---|
| `SLIMESEEKER_BUILD_TESTS` | `ON` | 构建单元、集成与回归测试 |
| `SLIMESEEKER_BUILD_BENCHMARKS` | `ON` | 构建 `slimeseeker_bench` |
| `SLIMESEEKER_BUILD_SHARED` | `OFF` | 构建共享库而不是静态库 |
| `SLIMESEEKER_ENABLE_IPO` | `ON` | 工具链支持时为优化配置启用 IPO/LTO |
| `SLIMESEEKER_ENABLE_CUDA` | `ON` | 检测到 CUDA toolkit 时构建 CUDA 搜索后端 |
| `SLIMESEEKER_CUDA_ARCHITECTURES` | `toolchain` | CUDA 目标档位：`toolchain`、`native`、`release` 或显式 CMake 架构列表 |
| `SLIMESEEKER_BUILD_TUI` | `ON` | 构建无参数启动的交互式终端工作台 |
| `SLIMESEEKER_FTXUI_PROVIDER` | `fetch` | 使用固定源码包 `fetch` 或已安装的 `system` FTXUI |

例如，只构建共享库与 CLI：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSLIMESEEKER_BUILD_SHARED=ON \
  -DSLIMESEEKER_BUILD_TUI=OFF \
  -DSLIMESEEKER_BUILD_TESTS=OFF \
  -DSLIMESEEKER_BUILD_BENCHMARKS=OFF
cmake --build build --parallel
```

安装到自定义前缀：

```sh
cmake --install build --prefix ./dist
```

需要和普通 Release 做无 LTO 对照时，可配置 `-DSLIMESEEKER_ENABLE_IPO=OFF`。项目不会全局使用 `-march=native`，平台专用指令只存在于对应的独立编译单元中。

### CUDA 构建与发行产物

默认 `toolchain` 档位尊重 `CMAKE_CUDA_ARCHITECTURES` 和 CUDA 工具链默认值，适合已有统一
工具链配置的工程。只在当前机器运行时可生成原生 SASS：

```sh
cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release \
  -DSLIMESEEKER_CUDA_ARCHITECTURES=native
cmake --build build-cuda --parallel
```

正式 Linux CUDA 发行包使用 CUDA 13 的 `release` 档位：

```sh
cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release \
  -DSLIMESEEKER_CUDA_ARCHITECTURES=release
cmake --build build-cuda --parallel
```

该档位包含 `sm_75/80/86/89/90/100/120` 的真实 SASS，并保留 `compute_120` PTX 供后续兼容
设备由驱动 JIT。它要求 CUDA 13.0 或更高版本。也可以传入 CMake 原生列表，例如
`-DSLIMESEEKER_CUDA_ARCHITECTURES="89-real;120-real;120-virtual"`。架构集合同时作用于静态库和
最终 device-link，避免链接阶段裁掉已经生成的 code object。

GitHub Release 中的 CUDA 包名为
`slimeseeker-<version>-linux-x86_64-cuda13.tar.gz`。包内 CUDA runtime 静态链接，不要求目标
机器安装 CUDA Toolkit 或动态 `libcudart`，但仍需要支持包内 SASS/PTX 目标的 NVIDIA 驱动。
CPU-only 包不加载 CUDA runtime，也不受此要求影响。

### TUI 依赖

默认构建会下载并校验固定的 FTXUI 7.0.3 源码包，只把它静态链接到 CLI；FTXUI 不会进入
`SlimeSeeker::slimeseeker` 的公共依赖或安装导出。需要完全离线且系统已经安装 FTXUI 时使用：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSLIMESEEKER_FTXUI_PROVIDER=system
```

只需要传统 CLI 和库时可设置 `-DSLIMESEEKER_BUILD_TUI=OFF`，此时配置阶段不会访问 FTXUI。
正式安装包包含 FTXUI 的 MIT 许可证，不要求目标机器安装额外终端库。

### macOS 首次运行提示

当前 GitHub Release 中的 macOS 二进制尚未经过 Apple Developer ID 签名与 notarization。通过浏览器下载后，macOS Gatekeeper 可能提示 Apple 无法验证 `slimeseeker` 是否包含恶意软件。如果文件确实来自本项目的[官方 Release](https://github.com/colorcard/SlimeSeeker/releases)，可以只对解压后的可执行文件移除 quarantine 属性：

```sh
xattr -d com.apple.quarantine /path/to/slimeseeker
```

请把 `/path/to/slimeseeker` 替换为实际可执行文件路径，然后重新运行。不要对来源不明的文件执行此命令，也不建议对整个下载目录递归移除 quarantine；后续版本计划接入 Developer ID 签名与 Apple notarization，从发布流程上消除该提示。

## 命令行用法

在交互式终端直接运行且不传参数会进入 TUI：

```sh
./build/slimeseeker
```

也可以显式使用 `./build/slimeseeker --tui`。stdin 或 stdout 不是终端时，TUI 会立即返回错误，
不会在 CI、管道或脚本中挂起。

TUI 配置页提供 seed、搜索范围、阈值、线程、CPU/CUDA 后端与结果保留策略。默认只保留全局
排序最佳的 1000 条结果，同时独立统计全部命中；K 可配置到 100 万。全量模式会保留每一条
结果，当最坏内存估算超过 512 MiB 时必须确认。搜索在后台线程运行，界面保持可响应并支持
取消；完成或取消后的结果可以分页查看区块坐标、计数、距离平方和对应方块边界。

CSV 导出在后台执行，列为 `x,z,count`。Top-K 模式只导出保留结果；取消或失败后的部分结果
使用带 `-partial` 的默认文件名并要求确认。覆盖已有文件时先完成临时文件，再以可恢复方式
替换原文件。

传统非交互命令行保持原有形式：

```text
slimeseeker [OPTIONS] SEED RANGE [THRESHOLD]
```

- `SEED` 是有符号 64 位 Minecraft 世界种子；
- 搜索中心范围为区块坐标 `[-RANGE, RANGE) × [-RANGE, RANGE)`；
- `THRESHOLD` 允许 `0..192`，默认值为 `45`。

完整选项：

| 选项 | 说明 |
|---|---|
| `-j, --threads N` | worker 数量，`0` 表示自动使用硬件并发数 |
| `-m, --backend auto\|scalar\|avx2\|neon\|cuda` | 选择 CPU/CUDA 后端，默认 `auto` |
| `-f, --format human\|csv` | 选择人类可读格式或 CSV，默认 `human` |
| `-u, --unordered` | 搜索时直接流式输出，不做最终排序 |
| `--top K` | 只保留全局排序最优的 K 条结果 |
| `-q, --quiet` | 关闭进度显示 |
| `-b, --benchmark` | 只报告端到端吞吐，不输出结果列表 |
| `--tui` | 显式进入交互式终端工作台，不能和其他参数组合 |
| `-v, --version` | 显示版本 |
| `-h, --help` | 显示帮助 |

默认结果排序为：圆环计数降序、到原点距离平方升序、`x` 升序、`z` 升序。`--unordered` 适合结果很多且不需要稳定顺序的场景；`--top K` 可以避免 CLI 为全量排序保留所有结果。按 `Ctrl-C` 会通过取消回调安全终止搜索。

常见示例：

```sh
# 默认后端和线程数，搜索 seed 0、[-10000,10000)²，阈值45
./build/slimeseeker 0 10000 45

# 静默输出 CSV
./build/slimeseeker -q -f csv 0 10000 45

# 固定8线程和标量后端，便于复现结果或性能
./build/slimeseeker --threads 8 --backend scalar 12345 2000 40

# 只保留排序最优的20个中心
./build/slimeseeker --top 20 12345 10000 45

# 不排序，命中结果按批次到达顺序直接输出
./build/slimeseeker --unordered -f csv 12345 10000 45

# 端到端吞吐测试，不输出命中列表
./build/slimeseeker --benchmark --threads 1 --backend scalar 0 5000 45
```

`auto` 只选择 CPU 搜索，并检测当前 CPU 能力，用短时对拍和中位数校准专用位图算子。只有 AVX2/NEON 算子结果与 scalar 完全一致且至少快 5% 时才会自动采用；显式请求硬件不支持的后端会返回 `SS_BACKEND_UNAVAILABLE`。

CUDA 需要使用 `-DSLIMESEEKER_ENABLE_CUDA=ON`（默认会在检测到 CUDA toolkit 时启用）。`auto` 不探测或初始化 GPU；显式使用 `--backend cuda` 时，CUDA 后端以四个 tile 为一批并用双 stream slot 重叠位图、17×17 圆环计数和结果回传。约 47 MiB 的单一 workspace 在进程内复用，并发 CUDA 搜索会有界排队；等待期间仍可取消。没有可用 CUDA 设备或 runtime 无法初始化时返回 `SS_BACKEND_UNAVAILABLE`。同一结果回调中不能重入 CUDA 搜索，嵌套调用返回 `SS_INTERNAL_ERROR`。

## C ABI 集成

唯一公共头文件是 `include/slimeseeker/slimeseeker.h`。以下 C 示例搜索一个小矩形，并统计所有命中结果：

```c
#include <stdio.h>
#include <slimeseeker/slimeseeker.h>

static int receive_results(void *context, const ss_result *results, size_t count) {
    size_t *total = (size_t *)context;
    *total += count;
    for (size_t i = 0; i < count; ++i) {
        printf("%d,%d,%u\n", results[i].x, results[i].z, results[i].count);
    }
    return 0;
}

int main(void) {
    ss_search_params_v1 params = {
        sizeof(ss_search_params_v1), 0,
        -1000, 1000, -1000, 1000,
        45, 0
    };
    ss_search_options_v1 options = {
        sizeof(ss_search_options_v1), 0, SS_BACKEND_AUTO, 1024
    };
    size_t total = 0;
    ss_callbacks_v1 callbacks = {
        sizeof(ss_callbacks_v1), &total,
        receive_results, NULL, NULL
    };

    ss_status status = ss_search(&params, &options, &callbacks);
    if (status != SS_OK) {
        fprintf(stderr, "search failed: %s\n", ss_status_string(status));
        return (int)status;
    }
    fprintf(stderr, "hits: %zu\n", total);
    return 0;
}
```

安装后的 CMake 消费方可以直接使用导出的目标：

```cmake
find_package(SlimeSeeker CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE SlimeSeeker::slimeseeker)
```

所有版本化结构都必须填写 `struct_size`。`options` 和 `callbacks` 可以传 `NULL` 使用默认值；结果数组只在当前 `on_results` 调用期间有效。结果、进度和取消回调全部由调用 `ss_search` 的线程串行触发，因此调用方无需为回调之间额外加锁。结果回调返回非零会得到 `SS_CALLBACK_ABORTED`；取消、参数错误、内存不足和后端不可用也有各自独立的状态码。

## 工作原理

### 精确史莱姆判定

每个候选区块先按 Minecraft Java Edition 的定宽整数规则构造 chunk seed。Java `int` 乘法必须保持 32 位二进制补码回绕，随后用 48 位 LCG 产生随机数，并精确执行 `Random.nextInt(10)`。实现保留 rejection sampling；极低概率的重试位于冷路径，但不会用有偏的单次取模近似替代它。

tile 内的 chunk seed 公式拆成只依赖 `x` 的 `xterm` 和只依赖世界种子、`z` 的 `zbase`。每列和每行只计算一次对应项，逐区块热路径主要保留加法、异或和一次 LCG 判定。

### 圆环与三段搜索管线

搜索引擎把区域切成最多 496×496 个候选中心的 tile，并生成四周额外扩展 8 格的最大 512×512 史莱姆位图。圆环几何为：

```text
1 < dx² + dz² <= 64
```

它包含 192 格，可以按行压缩成 20 个连续段。不同阈值下，预筛通过率相差很大，因此引擎使用三种结果完全一致的计数策略：

| 阈值 | 搜索策略 | 原因 |
|---:|---|---|
| `0..30` | 滑动圆环 | 每行完整计算首个圆环，横移时只更新20段左右边界 |
| `31..40` | 二维 SAT | 兼顾方框预筛与大量精确圆环查询 |
| `41..192` | 滑动方框 | 维护17行列和，跳过 SAT，只精确检查极少数通过者 |

### 并发、结果与后端

每个 worker 独占 map、SAT、列和及代数项 scratch，并通过原子索引动态领取 tile，避免热路径共享锁和静态分片尾部失衡。命中结果先进入 worker 局部批次，再通过有界队列交给调用线程；队列满时产生自然背压，内存不会随生产者速度无限增长。

完整搜索首先在 CPU 与 CUDA 描述符之间分派。CPU 搜索拥有 tile、worker、SAT 和有界结果队列，再在内部选择 scalar、AVX2 或 NEON 位图算子；CUDA 搜索拥有自己的融合 kernel、设备缓冲与结果回传，不模拟同步 CPU 位图接口。scalar 始终是所有实现的正确性基线。

所有完整搜索后端共享同一契约：回调由调用 `ss_search()` 的线程串行触发；取消、回调终止和不可用状态彼此独立；结果不能静默截断。未来 Metal/Vulkan 后端也必须在这一层接入，而不是向 CPU 搜索主体增加特殊分支。更详细的目录依赖、数据流和优化分析见[架构文档](docs/architecture.md)与[性能热点分析](docs/performance-hotspots.md)。

## Benchmark

独立基准程序同时测量 512×512 位图生成和端到端搜索：

```sh
./build/slimeseeker_bench 5000 1 45
```

参数依次为 `[range] [threads] [threshold]`，每行输出一条 JSON，包含请求/实际后端、候选数、命中数、耗时和吞吐。CUDA 会连续输出 `cold` 与 `warm`：前者包含首次设备探测和 workspace 分配，后者复用同一进程资源。建议在相同机器、编译器、构建配置与输入上交替运行多次并比较中位数，不要混合冷暖口径，也不要在普通 CI 中设置易受温度、频率和调度影响的绝对性能门槛。

以下为 Apple M1 Max（10 核、32 GiB）、macOS arm64、AppleClang 17、Release + IPO/LTO、scalar、seed `0` 的代表值：

| 候选数 | 阈值 | 线程 | 吞吐 |
|---:|---:|---:|---:|
| 1 亿 | 45 | 1 | 约 5.11 亿候选/秒 |
| 1 亿 | 45 | 10 | 约 34.4 亿候选/秒 |
| 1600 万 | 25 | 1 | 约 6600 万候选/秒 |
| 1600 万 | 20 | 1 | 约 5800 万候选/秒 |

高阈值主要受史莱姆位图与 LCG 限制；低阈值会把瓶颈转移到圆环计数和结果管线。因此不同阈值的吞吐不应直接视为 CPU 的单一固定能力。以上数字仅用于展示当前实现的量级，不是跨平台性能承诺。

## 测试与开发

常规 Release 验证：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitizer 验证：

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

测试覆盖领域数学、强制 rejection 冷路径、朴素圆环差分、策略边界、线程和后端一致性、C ABI 以及固定种子黄金结果。项目目录和依赖规则详见[架构文档](docs/architecture.md)。

## 下一步计划

以下清单描述计划中的用户能力和体验改进，优先级会根据正确性验证、跨平台测试和实际性能数据调整，不代表固定的版本或交付日期承诺。

- [ ] 提升 AVX2 与 NEON 后端的完整 LCG 批量计算能力，并继续保证与 scalar 结果逐位一致；
- [ ] 优化 CPU 核心拓扑识别和默认线程选择，改善混合性能核/能效核处理器上的自动配置；
- [ ] 根据搜索阈值和实际预筛通过率自适应选择搜索管线，减少用户手工调优需求；
- [ ] 改进低阈值密集结果场景的吞吐、内存占用和 Top-K 处理效率；
- [ ] 研究跨 tile 边缘复用，减少相邻搜索块之间的重复史莱姆判定；
- [ ] 扩充 Windows、Linux、macOS 以及 x86-64、arm64 的持续集成与性能回归覆盖；
- [ ] 提供更易集成的发布产物、安装说明和更多语言绑定示例；
- [ ] 评估 Metal、Vulkan 等可选 GPU 搜索后端，并保持与 CPU/CUDA 相同的精确结果语义。

## 致谢

感谢 [CITYWIDESIGN/SlimeRadar](https://github.com/CITYWIDESIGN/SlimeRadar)。该项目为 SlimeSeeker 早期的功能定义、Minecraft 规则核对、圆环搜索思路和性能分析提供了重要参考。
