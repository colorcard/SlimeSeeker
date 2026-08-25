# SlimeSeeker

SlimeSeeker 是一个跨平台的 Minecraft Java Edition 史莱姆区块密度搜索库与命令行程序。给定世界种子，它会在指定区块坐标范围内搜索 17×17 环形窗口中史莱姆区块数量达到阈值的中心点。

设计分层、数据流和后端分派约束见 [架构文档](docs/architecture.md)，当前性能瓶颈与后续
优化顺序见 [热点分析基线](docs/performance-hotspots.md)。

## 构建

需要 CMake 3.24+ 和支持 C++20 的 MSVC、GCC 或 Clang。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

默认同时构建 `slimeseeker`、`slimeseeker_unit_tests` 和 `slimeseeker_bench`。可通过
`SLIMESEEKER_BUILD_TESTS`、`SLIMESEEKER_BUILD_BENCHMARKS` 与
`SLIMESEEKER_BUILD_SHARED` 调整。
Release 与 RelWithDebInfo 默认在工具链支持时启用 IPO/LTO；需要生成无 LTO 对照构建时使用
`-DSLIMESEEKER_ENABLE_IPO=OFF`。

## 使用

```sh
./build/slimeseeker -q -f csv 0 10000 45
./build/slimeseeker --backend scalar --threads 8 12345 2000 40
./build/slimeseeker --top 20 12345 10000 45
./build/slimeseeker --unordered -f csv 12345 10000 45
./build/slimeseeker --benchmark 0 5000 45
```

搜索区域为 `[-RANGE, RANGE)²`。默认阈值为 45。默认结果顺序为 `count` 降序、到原点距离平方升序、`x` 升序、`z` 升序。

`auto` 后端会检测 CPU 能力，并仅在短时校准确认专用 AVX2/NEON 算子至少快 5% 时启用；可显式指定后端进行复现或基准测试。

## 库接口

公共 C ABI 位于 `include/slimeseeker/slimeseeker.h`。搜索结果以有界批次串行回调，不要求调用方处理并发回调，也不会使用固定总结果容量。返回非零的结果回调可终止搜索；另有进度与取消回调。

核心规则精确复刻 Java 48 位 LCG 与 `nextInt(10)` rejection sampling。环形掩码固定为 `1 < dx² + dz² <= 64`，共 192 个区块。

## 基准

```sh
./build/slimeseeker_bench 5000 0
```

每行输出一条 JSON，包含后端、范围、线程数、候选数、命中数、耗时和吞吐。性能数据用于同机、同编译器的版本比较，不在普通 CI 中设置易波动的绝对时间门槛。
