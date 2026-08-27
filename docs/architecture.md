# SlimeSeeker 架构

> 当前 CPU/CUDA 热点、线程扩展和优化优先级记录在
> [性能热点与优化路线](performance-hotspots.md) 中。

## 目录职责

```text
apps/cli/          传统 CLI 与 FTXUI 工作台，只通过公共 ABI 使用核心库
src/core/          Minecraft 数学规则与圆环几何
src/engine/        完整搜索分派、CPU tile、SAT、线程调度与结果管线
src/backends/      scalar、AVX2、NEON 位图算子与 CUDA 完整搜索实现
src/api/           公共 C ABI 的参数校验与异常隔离
include/slimeseeker/ 唯一对外头文件
tests/unit/        领域、搜索和控制契约的快速测试
tests/integration/ 纯 C 等跨边界集成验证
tests/regression/  大范围固定黄金结果回归
benchmarks/        分阶段及端到端性能基准
```

依赖方向固定为 `apps/api → engine → core + backends`；后端可以复用 core 的精确判定，
但 core 不反向依赖 engine、API 或具体后端。测试和基准可以通过私有 include 路径对白盒接口
做验证，这些内部头文件不会随安装包导出。

## 设计依据

搜索成本由“候选中心数”和“生成其周围史莱姆区块状态”决定。前者无法在不改变语义的情况下省略，因此架构围绕四个不变量设计：数学结果必须逐位兼容 Java、工作集应适合 CPU cache、并行任务之间不共享热数据、专用指令集不能污染通用构建。

C++20 用于内部实现，因为它同时提供定宽无符号回绕、RAII、标准线程以及对 AVX2/NEON intrinsic 的直接访问。公共边界使用版本化 C ABI，避免把 C++ 编译器 ABI、容器或异常暴露给调用方。

## 分层

1. 领域核心定义区块种子、Java 48 位 LCG、精确 `nextInt(10)`、环形几何和规范结果顺序。该层不依赖线程、CLI 或平台 API。
2. 完整搜索分派层选择 CPU 或 CUDA 描述符。每个描述符统一提供可用性查询和完整搜索入口，但拥有自己的执行与资源生命周期。
3. CPU 搜索把候选区域切成最多 496×496 个中心的 tile。每个 worker 独占位图和 SAT scratch，通过原子索引动态领取任务；CPU 内部位图算子可由 scalar、AVX2 或 NEON 替换。
4. CPU 的有界批次队列将 worker 与调用线程隔离；CUDA 以四个 tile 为一批并用双 stream slot 重叠计算与回传。两者都只从调用线程触发结果、进度和取消回调。
5. C ABI 负责输入尺寸和坐标验证、状态码与异常隔离。传统 CLI 与 TUI 只实现参数、信号、结果保留策略和显示。

Minecraft 26.2 群系重评分是 CLI/TUI 私有的可选第二阶段，不进入公共 ABI 或搜索 engine。其纯 C++
包装调用内置的 Cubiomes MIT 噪声与 fuzzy zoom 算子，再用从原版 26.2
`OverworldBiomeBuilder` 导出的参数点构建同构 6 路 R-tree。参数点在构建时已压缩为刷怪权重档位，
运行时不读取 Java、存档、数据包或外部生成文件。

结果回调对每个第一阶段命中同步评分，以有界堆只保留最终群系 Top-K；共享区块分数使用有界 FIFO
缓存。搜索结束后仅对保留结果生成逐方块权重，并在中心区块 16×16 个玩家位置上执行挂机几何扫描。
这一层可以依赖领域核心，但核心、engine 和公共库均不反向依赖世界生成实现。

## 数据流

```text
世界种子、矩形边界
        ↓
完整搜索后端分派
        ├─ CPU 搜索 → 动态 tile/worker → scalar/AVX2/NEON 位图算子
        │                              → 滑动圆环/SAT/滑动方框
        │                              → 有界结果队列
        └─ CUDA 搜索 → 4 tile批量位图与前缀和 → 圆环 kernel → 双缓冲异步回传
        ↓
调用线程串行 C 回调 → CLI/TUI 流式、全量或 Top-K 结果策略
```

传统 CLI 在主线程同步调用 `ss_search()`。TUI 的 FTXUI 事件循环固定留在主线程，搜索由一个
专用 `std::jthread` 调用同一公共 ABI。回调只更新原子进度和搜索线程独占的结果收集器；界面
最多每 100 ms 读取一次快照，不把逐批结果变成无界 UI 事件。搜索完成并按规范顺序排序后，
结果所有权一次性交给界面。退出运行中的 TUI 会先请求取消并等待搜索线程收敛，再恢复终端。

TUI 的区块密度模式默认用有界 Top-K 队列保留最佳结果，也允许用户明确选择全量模式；群系
模式则在搜索回调中复用 `BiomeScorer`，同步重评分所有命中并只保留最终群系 Top-K，随后在
后台完成挂机点扫描。群系结果页按评分所用的相同区块中心预筛和逐方块三维距离条件，生成
17×17 的史莱姆区块/挂机圆环覆盖模型；FTXUI 仅负责把相邻两行压为终端半块像素。CSV 导出
使用独立线程读取已经不可变的结果数组，通过同目录临时文件和备份替换目标，因而不会阻塞
界面或把取消的写入伪装成完整文件。FTXUI 只私有链接可执行程序，不改变公共库 ABI、安装
目标或依赖方向。

SAT 使用 `uint16_t` 环绕运算。虽然整张 512×512 图的总和可能超过 65535，但任一查询矩形最多 289 格；模 2¹⁶ 的四项差分仍精确得到该局部和。

每个 worker 在启动时一次性分配最大 tile 的 map、SAT 和 xterm scratch，后续 tile 只覆写有效区域。
SAT 复用时仅清零首行和每行首列，其余单元都会在融合构建中被覆盖，避免重复全缓冲清零。

## 后端与分派

项目刻意保留两层不同粒度的后端接口：

- `SearchBackend` 表示完整搜索生命周期。CPU 与 CUDA 都在此层注册 `available()` 和 `search()`；`search_impl()` 只选择描述符、检查可用性并调用搜索入口。
- `BuildMapFn` 只表示 CPU 内部的同步位图算子。scalar、AVX2 和 NEON 输入输出一致，由 `search_cpu_impl()` 选择；GPU 不需要通过主机指针返回中间位图。

CPU 描述符拥有 tile 划分、worker、SAT scratch、有界结果队列、进度和取消。CUDA 描述符拥有设备选择、设备内存、融合 kernel、结果压缩与主机回传。新增 Metal 或 Vulkan 时应注册新的完整搜索描述符，不得在 CPU 搜索主体内增加后端条件分支，也不得为了复用 `BuildMapFn` 引入不必要的设备到主机位图拷贝。

CUDA 使用一个进程级惰性 workspace，固定包含两个 stream slot，每个 slot 最多容纳四个
tile。device、event 和 pinned host 缓冲在搜索之间复用，总量约 47 MiB；多个调用线程通过
租约串行使用该 workspace，等待期间仍可取消。每批的 count 先异步回传并验证容量，再把
实际结果直接从 pinned buffer 切片交给调用线程。CUDA 结果回调中不允许同线程重入 CUDA
搜索，嵌套调用会返回 `SS_INTERNAL_ERROR`，避免单 workspace 等待自身形成死锁。

`available()` 只判断实现是否编译、当前设备是否存在以及 runtime 是否可初始化。无 CUDA 构建提供同签名 stub，因此 engine 不包含 `SS_HAS_CUDA` 条件编译。显式选择不可用后端返回 `SS_BACKEND_UNAVAILABLE`。

CUDA 构建区分本机优化与可分发产物。`native` 只生成当前设备的真实 SASS；正式 CUDA 13
发行档位同时生成 `sm_75/80/86/89/90/100/120` SASS 和 `compute_120` PTX。架构列表同时设置
到库目标与顶层 device-link，防止最终链接按工具链默认架构裁掉静态库中的其他 code object。
当前 CUDA 设备代码集中在单一编译单元，不启用不必要的 relocatable device code；CUDA
runtime 静态链接，因此发行包运行时只依赖兼容的 NVIDIA 驱动，不依赖目标机器安装 Toolkit。
CI 使用 `cuobjdump` 检查所有 SASS 与 PTX，并用动态依赖检查阻止意外引入 `libcudart`。

`auto` 的语义保持 CPU-only：它不会枚举或初始化 GPU，而是在 CPU 搜索内部检查 ISA 能力，并对 scalar 和专用位图算子做固定工作量校准。各 ISA 文件单独使用 `-mavx2` 或对应平台默认 arm64 NEON 编译，通用目标不使用 `-march=native`；只有专用实现的中位耗时至少低 5% 才采用它，选择结果在进程内缓存。

每个完整搜索后端都必须满足相同契约：

- 结果、进度和取消回调只由调用 `ss_search()` 的线程串行触发，结果数组仅在当前回调期间有效。
- 回调非零返回、取消、不可用、内存不足和内部错误映射到各自状态码。
- 半开区间、阈值和计数语义与 scalar 逐项一致；后端内部顺序不作承诺。
- 设备缓冲不足时必须扩容、分片或返回失败，不能以 `SS_OK` 静默截断结果。

## 正确性与资源边界

- 搜索中心采用半开区间，外扩 8 格前验证 `int32_t` 边界。
- 阈值范围为 0..192；所有面积和 tile 数先使用 `uint64_t` 计算。
- 库不设置总结果上限。队列内存有界；全量排序产生的内存成本属于 CLI 的显式结果策略。
- 回调终止、用户取消、参数错误、后端不可用和内存不足使用不同状态码；不会以成功状态返回截断结果。
- 普通测试包含独立朴素差分和三个完整黄金种子；Sanitizer 与跨平台构建用于发现未定义行为和平台假设。
