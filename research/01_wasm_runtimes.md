# WebAssembly 运行时引擎全景调研

> 调研日期：2026-09-21。范围：Wasm 核心技术栈、分层编译范式、六大主流引擎架构拆解、内存模型、工业界实战。所有关键论断均附来源 URL；标注「官方/源码可证」或「社区/二手转述」。

---

## 1. WebAssembly 总体技术栈

### 1.1 二进制格式与模块结构

Wasm 模块是部署、加载与编译的基本单位。一个模块在逻辑上聚合：类型段（types）、导入段（imports）、函数声明（func）、表（tables）、内存（memories）、全局变量（globals）、导出（exports）、起始函数（start）、元素段（elem）与数据段（data）。规范原文明确："Modules … collect definitions for types, tags, and globals, memories, tables, functions"（[webassembly.github.io/spec/core/syntax/modules.html](https://webassembly.github.io/spec/core/syntax/modules.html)，官方规范）。

二进制层由若干 **section** 组成，每个 section 以单字节 ID + LEB128 长度 + payload 编码。整数采用 **unsigned/signed LEB128**（可变长度小端编码），这是整个二进制格式压缩密度的关键。函数体内部是 **栈式字节码**（stack machine）：没有显式寄存器操作数，操作数由一个隐式操作数栈传递。这一设计直接决定了所有后端编译器的输入形态——无论是 Liftoff 的虚拟操作数栈镜像，还是 Cranelift 的 SSA lowering。

模块头部为 `\0asm` magic + version，随后按规范顺序排列 12 个 section。校验器可以流式地逐 section、逐函数并行校验（如 wasmparser 的设计目标："designed for parallel validation of functions as they are received"，[wasmparser validator.rs](https://raw.githubusercontent.com/mozilla-firefox/firefox/main/third_party/rust/wasmparser/src/validator.rs)，源码可证）。

### 1.2 校验（Validation）

校验是 Wasm 安全模型的基石。规范定义："Validation checks a number of well-formedness conditions … type checking of functions and the instruction sequences"（[W3C Core Spec](https://www.w3.org/TR/wasm-core-1/)，官方）。校验器维护一个 **类型化栈机**：对每条指令检查其输入栈顶类型是否匹配、控制流块类型（block/loop/if）是否闭合、local/global/table/memory 索引是否越界、导入导出签名是否一致。校验通过的模块才被允许实例化；这一步是沙箱"类型安全"承诺的来源——所有越界访问、类型混淆都在校验期被拒绝，运行时只保留内存边界检查（bounds check）这一类 trap。

工程上，校验与编译可以流水线并行：V8 的 streaming compilation、Wasmtime 的 `wasmparser` 增量校验都遵循"边下载边校验边编译"。

### 1.3 执行语义

Wasm 是纯栈式、确定性、无隐式 GC 的指令集。指令分四类：
- **数值指令**：i32/i64/f32/f64 的算术、比较、转换；
- **参数/局部变量**：local.get/set/tee、global.get/set；
- **内存指令**：load/store 带 alignment 提示，地址为 `i32`（MVP）或 `i64`（memory64）；
- **控制流**：block/loop/if/else/br/br_if/call/call_indirect/return。

关键语义点：
1. **Trap 语义**：除零、整数溢出除法、越界 load/store 直接 trap，而不是返回 NaN 或包装。这使嵌入式后端可以用硬件异常（如 x86 `#GP`、AArch64 alignment fault）复用为 trap，无需显式检查。
2. **确定性**：同一模块在任何平台上语义一致（不依赖 host 的 `errno`、信号、浮点舍入模式），这是"可移植部署单元"承诺的基础。
3. **无隐式线程/无 OS 概念**：MVP 下模块是单线程的；并发通过 threads 提案的 shared memory + atomics 实现（见 §4.3）。

### 1.4 Embedding API 与 WASI

核心规范不规定如何与宿主机交互。每个 embedder 自行定义宿主 API：
- **Web**：`WebAssembly.instantiate`、`WebAssembly.Memory`、`WebAssembly.Table`，由 JS 层提供（[JS Interface](https://webassembly.github.io/spec/js-api/)，官方）；
- **wasm-c-api**：W3C 社区组定义的 C ABI，供非浏览器嵌入（[WASI.dev 介绍](https://wasi.dev/)，社区索引）；
- **WASI**：系统接口提案，把文件、时钟、随机数、sock 等能力以能力（capability）方式导入，是 server-side Wasm 的标准 ABI。

embedder 负责：提供线性内存与表、实现导入函数、处理 trap、决定如何调度执行。引擎本身不"管 OS"，这是 Wasm 与 JVM/CLR 的本质区别。

---

## 2. 分层编译范式

Wasm 引擎普遍采用"多 tier 编译"来同时优化 **启动时间** 与 **峰值吞吐**。四层范式如下：

### 2.1 解释器（Interpreter）

直接解码字节码、驱动一个显式栈机。优点：启动零编译开销、内存占用最小、可移植性极强（不依赖可写内存页，因此能跑在 iOS App Sandbox 等禁止 JIT 的环境）。缺点：典型性能比原生 JIT 慢 10~50×。代表：Wasm3、WAMR classic/fast interpreter、Wasmi、Wasmtime 的 Pulley。

工程技巧：寄存器化字节码（WAMR 把栈式字节码翻译为自定义寄存器式字节码，减少 VM 内派发开销，见 [ACM 综述](https://dl.acm.org/doi/pdf/10.1145/3731451)）、computed goto / tail-call 派发（Wasmi 2.0 用手写汇编的 dispatch 表，[wasmi 2.0 博客](https://wasmi-labs.github.io/blog/posts/wasmi-v2.0/)）、in-place 解释（[arXiv 2205.01183](https://arxiv.org/pdf/2205.01183v1.pdf)）。

### 2.2 Baseline JIT（快速出码）

单趟、无优化、近乎线性时间地把每条 Wasm 指令直接 lowering 到 host 机器码。目标是 **最短 time-to-first-execution**，不追求峰值。代表：V8 Liftoff、Wasmtime Winch、Wasmer Singlepass、JSC BBQ。

Liftoff 的策略（[V8 官方博客 Liftoff](https://v8.dev/blog/liftoff)，官方；中文镜像 [xenojoshua](https://xenojoshua.com/posts/2018/08/liftoff)）：
- 维护一个与规范一致的虚拟操作数栈；操作数尽量留在寄存器/临时栈上，只有溢出才写回内存；
- 寄存器分配采用 **线性扫描 + 固定帧槽**，不做活跃性分析；
- 函数进入时一次性分配栈帧，参数与 local 映射到固定槽位；
- 出码速度比 TurboFan 快约 10×，生成的代码比 TurboFan 慢约 4×（官方数据）。这使一个 70MB 模块的启动从 TurboFan 的数秒级压到 1~2 秒（[社区分析 ursb.me](https://ursb.me/immersive/webassembly/)，二手转述但与官方数字一致）。

Winch 同理：单趟 visitor，无 IR，"emits native code very quickly … implementation is a lot simpler than Cranelift's"（[Bytecode Alliance 2023 年度总结](https://bytecodealliance.org/articles/wasmtime-and-cranelift-in-2023)，官方）。

### 2.3 优化 JIT（Optimizing JIT）

在函数被热执行后，异步或同步地用优化编译器重新编译，应用逃逸分析、内联、循环优化、寄存器分配、自动向量化、逃逸分析后的栈对象等。代表：V8 TurboFan/Turboshaft、Cranelift、Wasmer LLVM、JSC OMG/B3。

触发时机由采样/profiling 决定：V8 对 Wasm 通常在函数调用次数/循环回边次数超过阈值后 tier-up 到 TurboFan/Turboshaft。优化 tier 的编译成本高（Cranelift 对大函数仍需百毫秒级），但一旦出码，性能接近原生 C（Cranelift 输出约比 V8 TurboFan 慢 2%、比 LLVM 慢 14%，[cranelift.dev](https://cranelift.dev/)，官方）。

### 2.4 AOT（提前编译）

在部署期把 `.wasm` 编译成宿主原生对象文件/共享库，运行时只做加载与实例化。优点：运行时零编译开销、启动可预测、可审计；缺点：失去跨架构可移植性（产物与 host 绑定）、需要运行时（runtime）配合处理 trap 与内存映射。代表：Fastly Lucet、Wasmtime `cranelift-object`、WAMR `wamrc`。

### 2.5 Warmup 曲线差异

- **纯解释器**：立即开始执行，吞吐恒定且低；
- **Baseline JIT**：几十~几百毫秒内开始执行，吞吐低但远好于解释器；
- **Optimizing JIT**：开始执行有延迟（后台编译），热函数 tier-up 后吞吐达到峰值；
- **AOT**：首次执行即接近峰值，但部署期编译成本前移、产物不可跨架构。

工程上最成熟的方案是 **组合**：V8 = Liftoff + TurboFan/Turboshaft；Wasmtime = Winch + Cranelift；WAMR = fast-interpreter/AOT + Fast-JIT + LLVM-JIT tier-up；JSC = BBQ + OMG。

---

## 3. 主流引擎逐一拆解

### 3.1 V8：Liftoff + TurboFan → Turboshaft

**定位**：Chrome/Node.js/Deno/Bun(替代) 的 Wasm 引擎，也是工业界 tiering 最成熟的实现。

**架构图思路**：
```
.wasm binary
  → 解码器 + 校验器（流式）
  → Liftoff（baseline，单趟出码）
  → [profiling 采样]
  → Turbofan / Turboshaft graph builder
  → 中端优化（SEA-of-Nodes / CFG reducers）
  → Instruction selector + register allocator
  → 架构相关后端（x64/ARM64）
```

**关键 IR**：TurboFan 历史上用 Sea-of-Nodes（图 IR，节点=操作，边=依赖）；近年 Wasm 管线已整体迁移到新的 **Turboshaft**——CFG-based、phase pipeline 用 C++20 concepts 约束，"WebAssembly 走完整管线全程 Turboshaft"（[rosettalens 译文](https://rosettalens.com/s/ko/leaving-the-sea-of-nodes)，二手转述；源码结构见 [deepwiki V8](https://deepwiki.com/v8/v8/3.3-turboshaft-and-turbofan:-optimizing-compilers)）。

**CodeStubAssembler (CSA)**：V8 用它编写 builtin（如 Wasm runtime 调用、trap handler、内存边界检查 stub），是一门低层、可移植的内部汇编 IR，被 Turbofan/Turboshaft 的 lowering 阶段直接调用。Wasm 的 prologue/epilogue、堆栈检查、内存基址加载都由 CSA stub 提供。

**Maglev 对 Wasm 的影响**：Maglev 是 V8 2023 年引入的 JavaScript 中层 JIT（在 Sparkplug 与 TurboFan 之间），主要服务 JS 热路径；对 Wasm 没有直接对应层——Wasm 仍是 Liftoff → 优化层两层。但 Maglev 的存在挤压了 V8 团队把更多优化预算投入 Turboshaft 中端，Wasm 间接受益于 Turboshaft 取代 Sea-of-Nodes 后的更清晰 phase 体系。

**优化手段**：Liftoff 出码后，Wasm 函数被 profiler 采样（回边计数、调用计数），热函数异步重编译到 Turboshaft；优化包括内联小函数、循环不变量外提、load/store 消除 bounds check（利用 proven 范围）、SIMD 自动/显式 vectorization、tail call。

**授权**：BSD-3-Clause。**典型 embedding**：Chrome、Node.js、Deno、Cloudflare Workers、Fastly Compute（历史）、Electron。

### 3.2 Wasmtime / Cranelift（Bytecode Alliance）

**定位**：Bytecode Alliance 旗舰 server-side Wasm 运行时，Rust 编写，WASI 与 Component Model 的参考实现。

**架构图思路**：
```
.wasm
  → wasmparser（流式解码+校验）
  → Wasmtime context（Engine/Store/Module/Instance）
  → 编译后端二选一：
      ├─ Winch（baseline，单趟，无 IR）
      └─ Cranelift（优化 JIT/AOT）
           → CLIF IR（SSA + CFG）
           → ISLE 指令选择（.isle 声明式重写规则）
           → regalloc2 寄存器分配
           → 机器码 emit
  → Runtime：内存映射、trap 处理、externref/GC 屏障
```

**Cranelift IR（CLIF）**：Rust 写的、与 LLVM 同构但更轻量的后端 IR。设计哲学："generate fast-enough code"——不追求 LLVM 级别的激进优化，而追求编译速度、可预测性与 **可验证性**。官方基准：输出比 V8 TurboFan 慢约 2%、比 LLVM 慢约 14%（[cranelift.dev](https://cranelift.dev/)，官方）。

**指令选择：ISLE**。Cranelift 不用硬编码 C++ 指令选择逻辑，而是用领域特定语言 **ISLE（Instruction Selection Lowering Expressions）**：每个架构一组 `.isle` 文件，以 term-rewriting 规则把 CLIF 模式映射到机器指令（[arXiv 2606.26977](https://arxiv.org/html/2606.26977v1)；[deepwiki Wasmtime](https://deepwiki.com/bytecodealliance/wasmtime/4.1-cranelift-compiler)）。这种"声明式 + 可形式化验证"的选择源于安全事件：2023 年 CVE 中 Cranelift 指令选择错误让 guest 能越界访问 6~34 GB 地址空间（[Cornell veri-isle 论文](https://www.cs.cornell.edu/~avh/veri-isle-preprint.pdf)），此后团队把 ISLE 用作可验证指令选择的基础。

**寄存器分配**：使用 **regalloc2**（与其他后端共享的 SSA 寄存器分配器，线性扫描 + liveness）。

**特权/沙箱模型（OS-like）**：Wasmtime 不把 Wasm 当作"插件"，而是当作一个 **微进程**：线性内存通过 `mmap` 映射为独立虚拟地址区间，guard page 提供边界检查；调用 guest 通过受控的 entry stub；trap 用 host 信号（SIGSEGV/SIGBUS）或显式检查处理；Component Model 进一步把 Wasm 当作"能力安全的微内核进程"。这种"OS-like"抽象是它与 V8（在浏览器内共享堆）的关键差异。

**Winch 与 Pulley**：Winch 是 baseline 编译器（[BA 2023 总结](https://bytecodealliance.org/articles/wasmtime-and-cranelift-in-2023)），Wasmtime 35 起支持 AArch64（[BA 文章列表](https://bytecodealliance.org/articles/)）；Pulley 是新增的解释器后端，用于不支持 JIT 的平台（[HybridServe 论文表](https://dl.acm.org/doi/pdf/10.1145/3774899.3775011)）。

**授权**：Apache-2.0 与 LLVM-exception 双许可。**典型 embedding**：Fastly Compute、Fermyon Spin、Shopify Functions、Deno Deploy（部分场景）、Mozilla RLBox。

### 3.3 WAMR（Intel iwasm）

**定位**：Intel 主导、现归 Bytecode Alliance 的嵌入式优先 Wasm 运行时，C 编写。

**架构图思路**：
```
.wasm
  → iwasm VMcore（统一加载器）
  → 执行后端四选一/可组合：
      ├─ classic interpreter（栈式字节码，最小内存）
      ├─ fast interpreter（寄存器化字节码，更快）
      ├─ Fast JIT（自定义轻量 JIT）
      └─ LLVM JIT（基于 LLVM，峰值性能）
  → wamrc（AOT 编译器，离线把 .wasm 编成 .aot）
  → 运行时：libc-builtin / libc-WASI、内存分配器、线程管理
```

**关键工程细节**：
- **AOT 文件格式**：`wamrc` 把 Wasm 编译成自定义 `.aot` 包（内含原生代码 + 元数据），由 iwasm 内置的 AOT loader 加载，支持 Linux/Windows/macOS/Android/SGX/MCU（[ESP Component 说明](https://components.espressif.com/components/espressif/wasm-micro-runtime/versions/2.2.0~2)）。AOT 产物 **不依赖完整 libc**，而是依赖 WAMR 自带的 mini-C runtime（`libc-builtin` 约 3.7K，`libc-WASI` 约 21.4K，同上）。
- **分层 JIT**：先跑 Fast JIT，热 tier-up 到 LLVM JIT（[Espressif 组件页](https://components.espressif.com/components/espressif/wasm-micro-runtime/versions/2.2.0~2?language=en)）。
- **占用**：VMCore 约 50K（AOT）/80K（解释器）；hello-world 运行可低至 16K DRAM（[W3C WAMR 介绍 PDF](https://www.w3.org/2020/08/29-chinese-web/wamr.pdf)，官方演讲）；独立学术测量 "runtime binary size of 50 KiB for AoT"（[TWINE 论文 arXiv 2103.15860](https://arxiv.org/pdf/2103.15860.pdf)）。
- **wasm-c-api**：原生支持，是嵌入式/TEE/MCU 场景最常见的 C ABI。

**授权**：Apache-2.0。**典型 embedding**：ESP32/ESP-IDF、SGX  enclaves、OpenVela、AliOS Things、各种 IoT/TEE 场景。

### 3.4 Wasmer

**定位**：通用型独立 Wasm 运行时，Rust 编写，最大特点是 **可插拔多编译器后端**。

**三种后端对比**（[Wasmer 官方文档](https://docs.wasmer.io/runtime/features/)，官方；[crates.io wasmer](https://crates.io/crates/wasmer/7.3.0-rc.1)）：

| 后端 | 编译速度 | 运行性能 | 适用 |
|---|---|---|---|
| Singlepass | 极快（单趟、无优化） | 较慢，但抗 JIT-bomb | 区块链智能合约、serverless 冷启动 |
| Cranelift | 中 | 较好 | 开发期 |
| LLVM | 慢 | 最好（近原生，约比另两者快 ~50%） | 生产 |

实测数据：编译 `qjs.wasm`，Singlepass 102ms、Cranelift 295ms、LLVM 更慢（[Wasmer Python embedding 博客](https://wasmer.io/es/posts/wasmer-python-embedding-1_0)，官方）。Wasmer 5.0 升级到 LLVM 18 与最新 Cranelift，CoreMark 比 v4.4 再快约 8%（[Wasmer v5 发布](https://wasmer.io/posts/introducing-wasmer-v5)，官方）。

**Singlepass 的设计哲学**：单趟 forward pass，不构造 IR，不做优化。这同时带来两个好处——(1) 编译时间与模块大小近似线性，适合"不可信输入不能让编译器爆炸"的区块链场景；(2) 不做激进优化意味着不会因优化器 bug 产生 JIT 安全漏洞。

**授权**：MIT。**典型 embedding**：Python/Ruby/PHP/Go 嵌入 SDK、区块链合约运行时、桌面应用插件系统、Wasmer Edge。

### 3.5 JavaScriptCore（JSC）：BBQ + OMG / B3

**定位**：Safari/WebKit 的 JS 引擎，Bun 的运行时核心。

**Wasm 路径**（[WebKit 博客 "Assembling WebAssembly"](https://webkit.org/blog/7691/webassembly/)，官方）：
- **BBQ（Build Bytecode Quickly）**：baseline tier，把 Wasm 编译成 JSC 自己的 B3 输入，快速出码；
- **OMG（Optimized Machine-code Generator）**：优化 tier，在 BBQ 字节码上做优化后交给 B3；
- 两层都依赖 **B3（Bare Bones Backend）** 作为底层优化器。B3 于 2016 年取代 LLVM 成为 FTL 的后端，理由是"LLVM 不是为动态语言优化挑战设计的"（[WebKit B3 介绍](https://webkit.org/blog/5852/introducing-the-b3-jit-compiler/)，官方）。

B3 自身有：B3 IR（SSA + CFG）、B3 lowering、Air（Assembly IR）、在 Air 上做指令选择与寄存器分配。对 Wasm 而言，BBQ 出码后，热函数 OMG 重编译并利用 B3 的优化。

**IPInt 解释器**：WebKit 近年新增 IPInt 作为快速启动的解释层，已覆盖 SIMD、v128 local/global、tail call、OSR to BBQ、异常处理、WasmGC struct（[JetStream 3 博客](https://webkit.org/blog/17899/introducing-the-jetstream-3-benchmark-suite/)，官方）。

**Bun 选择 JSC 的工程理由**：
- **启动快、内存小**：JSC 比 V8 启动更快、常驻内存更低；Bun 官方称 "JavaScriptCore usually starts and runs faster than V8"（[Bun Runtime 文档](https://bun.com/docs/runtime)，官方）；社区测到 Bun 冷启动约 8ms vs Node 约 45ms（[josenaldo 分析](https://josenaldo.com.br/codex-technomanticus-site/03-dominios/tecnologia/20---bun-como-runtime-e-toolkit-all-in-one)，二手转述）。
- **单文件分发**：Bun 用 Zig 把 JSC + 自家 runtime + bundler + test runner 静态链成单个可执行文件。
- **权衡**：JSC 峰值吞吐略低于 V8，但 serverless/CLI 场景更看重启动与内存。

**授权**：LGPL-2.0 / LGPL-2.1（WebKit 系）。**典型 embedding**：Safari、Bun、各类 iOS/macOS 应用内嵌。

### 3.6 其他值得对比的引擎与思路

**Wasmi**（纯 Rust 解释器）：定位"简单、正确、确定性、跨平台 no_std、抗 JIT-bomb"。API 松散镜像 Wasmtime，可作 drop-in 替换（[docs.rs wasmi_wast](https://docs.rs/crate/wasmi_wast/2.0.0-beta.8)，官方）。2.0 用手写汇编的 tail-call 派发，性能比旧版快约 2.2×，在许多负载上与 Wasm3/Stitch 持平或更快（[wasmi 2.0 博客](https://wasmi-labs.github.io/blog/posts/wasmi-v2.0/)；[Web And IT News 报道](https://www.webanditnews.com/2026/09/03/wasmi-2-0-delivers-2-2x-speed-boost-for-webassembly-in-constrained-environments/)）。适合 iOS 沙盒、禁止 JIT 的云环境、合约解释执行。

**WasmJIT / Monaco 类项目**：学界有大量教学性 JIT（如基于简单 AST 遍历+机器码 emit 的课程项目）；工业界没有名为"Monaco"的主流 Wasm 引擎（与 VS Code 的 Monaco editor 无关联），此处不展开。真正值得借鉴的是 **LuaJIT 的 trace compilation** 思路。

**LuaJIT trace 对 Wasm 的借鉴**：LuaJIT 的 Trace Compiler（Tracing JIT）记录热循环路径，把动态类型假设 specialize 成机器码。Wasm 本身是静态类型、校验期已确定所有类型，因此 trace 的"类型特化"红利不大；但 trace 的 **OSR（on-stack replacement）+ 去优化** 思想被 V8/JSC 借鉴——baseline 代码里插入 profiling 计数，热路径 OSR 到优化代码，假设失效时 deopt 回 baseline。目前没有任何主流 Wasm 引擎采用完整 tracing JIT，主要原因是 Wasm 的确定性 + 静态类型使方法内联/循环优化在离线 IR 上做更划算，trace 的记录开销不划算。

**Wasm3**：另一个高性能解释器，hand-written assembly dispatch，常作为"无 JIT 环境下最快解释器"基准（[arXiv 2205.01183](https://arxiv.org/pdf/2205.01183v1.pdf) 对比了 wasm3/wasmer）。

---

## 4. 内存模型

### 4.1 线性内存（Linear Memory）

Wasm 没有堆/栈/全局区的概念，只有一块或多块 **线性内存**：一个连续字节数组，按 64KB 页（page）增长，可声明 min/max。load/store 指令以 `i32` 偏移（MVP）或 `i64` 偏移（memory64）寻址，边界检查由引擎保证，越界即 trap。每个 Instance 拥有独立内存，默认不共享；这天然实现了模块间隔离（[MDN Using the JS API](https://developer.mozilla.org/en-US/docs/WebAssembly/Guides/Using_the_JavaScript_API)）。

工程实现：引擎通常用 `mmap` 映射一块虚拟地址空间，尾部留 guard page，硬件缺页即 trap，省去显式边界检查。这是 Wasm 内存安全性能近乎原生的核心原因。

### 4.2 Table、Globals、参考类型

- **Table**：一个不透明引用的数组（MVP 仅 funcref），用于 `call_indirect` 间接调用。函数指针不能直接放在线性内存里（安全需要），必须经 Table 间接化。
- **Globals**：每个全局是一个类型化值，可 mutable/immutable，由常量表达式初始化。mutable globals 在 threads 提案下用于跨实例共享（[规范 Globals 节](https://webassembly.github.io/spec/core/bikeshed/)）。
- **参考类型（Reference Types 提案）**：引入 `funcref`（任意函数引用）与 `externref`（embedder 持有的不透明对象引用）。"Reference types are opaque, meaning that neither their size nor their bit pattern can be observed"（[Wasm 2.0 规范 PDF](https://webassembly.github.io/custom-descriptors/versions/core/WebAssembly-2.0.pdf)）。`externref` 是 Wasm 与宿主对象交互的安全桥——宿主可以把任意 JS 对象/主机指针包成 externref 传进去，Wasm 无法解引用其位模式，只能在 table/global 中持有。

### 4.3 共享内存与原子操作

**Threads 提案**分两部分（[MDN Understanding text format](https://developer.mozilla.org/en-US/docs/WebAssembly/Guides/Understanding_the_text_format)）：
1. **Shared Memory**：`SharedArrayBuffer` 跨 Worker 共享；
2. **Atomic 内存指令**：`memory.atomic.*`、`wait`/`notify`、`RMW`（read-modify-write）。

内存模型沿用 C++/Java 的 RC11 风格 seqlock + release/acquire + sequentially consistent 操作，需要与 host 的内存序语义严格对齐。浏览器实现还需配合 COOP/COEP 等隔离头，避免 Spectre 类跨域泄露。

### 4.4 Memory64

长期缺失的 64 位地址支持。Memory64 提案把线性内存索引从 `i32` 扩到 `i64`，允许单模块使用超过 4GB 内存。**状态**：Firefox 134、Chrome 133 起正式发布（[SpiderMonkey 博客 "Is Memory64 actually worth using?"](https://spidermonkey.dev/blog/2025/01/15/is-memory64-actually-worth-using.html)，官方）；V8/Wasmtime/WAMR 均已跟进（[InfoQ 2023 年度回顾](http://m.toutiao.com/group/7324979199884984859/)，二手转述）。实际收益因平台而异：64 位索引让内存访问指令变宽、cache line 压力上升，SpiderMonkey 实测在许多工作负载上 **不一定更快**，主要价值在大内存/数据库场景。

---

## 5. 工业界实战

### 5.1 Cloudflare Workers：V8 Isolate + 启动优化

Cloudflare Workers 不跑容器，而是跑 **V8 Isolate**——"A single instance of the runtime can run hundreds or thousands of isolates"，每个 isolate 内存完全隔离，毫秒级启动，"约比典型容器快 100×、省 10~100× 内存"（[Cloudflare 官方文档](https://developers.cloudflare.com/workers/reference/how-workers-works/#isolates)；[Cloudflare 博客 Dynamic Workers](https://blog.cloudflare.com/dynamic-workers/)）。

关键优化：
- **Wasm 线性内存快照**：Python Workers（Pyodide）在部署时执行顶层初始化代码，然后拍 Wasm 线性内存快照，请求到来时直接恢复快照，跳过 Python 启动（[How Python Workers Work](https://developers.cloudflare.com/workers/languages/python/how-python-workers-work)）。这本质是把 Wizer 思路产品化。
- **GC 调优**：早期 Workers 因手动调 V8 young generation 大小导致 GC 过度频繁；改回 V8 自适应后，CPU 基准提升约 25%（[Cloudflare CPU 基准博客](https://blog.cloudflare.com/unpacking-cloudflare-workers-cpu-performance-benchmarks/)）。

### 5.2 Fastly Lucet → Wasmtime

Lucet 是 Fastly 2019 年开源的原生 Wasm 编译器/运行时，**主打 AOT**：把 Wasm 编译成原生代码，运行时只管资源与 trap，"dramatically simplifies the design and overhead of the runtime compared to the just-in-time strategy"（[Fastly Lucet 发布博客](https://www.fastly.com/blog/announcing-lucet-fastly-native-webassembly-compiler-runtime)）。Lucet 测到的实例化时间约 **34~35 微秒**（[notist 转述](https://notist.co/patrickhamann/uEw4zt/webassembly-to-the-browser-and-beyond)；[Fastly 资料](https://www.fastly.com/blog/announcing-lucet-fastly-native-webassembly-compiler-runtime)）。

2020 年 Fastly 决定把 Lucet 团队与工程并入 Wasmtime/Cranelift，"make a stronger compiler together"（[Fastly 官方博客](https://www.fastly.com/blog/how-lucet-wasmtime-make-stronger-compiler-together)）。今天 Fastly Compute 跑在 Wasmtime + Cranelift 上，冷启动仍稳定在亚毫秒级（[nordiso 综述](https://www.nordiso.com/blog/webassembly-production-use-cases-performance-benchmarks)）。Lucet 同时是 Mozilla RLBox 沙箱的后端，用于沙箱化第三方 C/C++ 库（[Fastly 日文博客](https://www.fastly.co.jp/jp/blog/how-fastly-and-developer-community-invest-in-webassembly-ecosystem)）。

### 5.3 Shopify Functions

Shopify 把 checkout/折扣/配送的商家自定义逻辑编译成 Wasm，跑在 Wasmtime 沙箱里，严格 CPU/内存预算，每个函数 **5 毫秒硬预算**，每日执行数百万次，中位延迟亚毫秒（[nordiso 综述](https://www.nordiso.com/blog/webassembly-production-use-cases-performance-benchmarks)；[AIO APEX](https://aioapex.com/es/blog/webassembly-beyond-browser-2026)）。这是"多租户 + 严格时延预算 + 强隔离"场景的标杆——容器方案要达到同等隔离密度成本高得多。

### 5.4 Deno

Deno 基于 V8 + Rust + Tokio，**默认安全**：无 `--allow-*` 授权则无文件/网络/环境/子进程访问（[Deno 权限文档](https://deno.land/manual@main/basics/permissions)；[Deno 官网](https://deno.com/)）。Wasm 在 Deno 中作为一等公民，但 Deno 的安全边界不在 Wasm 沙箱，而在 **宿主权限模型**——Wasm 本身仍受 V8 沙箱保护，但 Deno 额外用 OS 能力（seccomp/chroot/cgroups）或 Deno Deploy 的 Linux microVM 做 defense-in-depth（[Deno Sandbox 博客](https://deno.com/blog/introducing-deno-sandbox)）。

### 5.5 Wasmer Embedding 与 SYSROOT

Wasmer 提供 Python/Ruby/PHP/Go/Rust 多语言 embedding SDK，把 `.wasm` 当作可分发插件单元。SYSROOT 实践指：用 `wasm32-wasi` 工具链在一个 sysroot 下预编译所有依赖，再链接成单个 `.wasm`，避免 Emscripten 那套庞大的 mock 依赖——Cloudflare 明确表示"opted for native Rust whenever possible"以避开 Emscripten 的臃肿（[Cloudflare Rust 标签](https://blog.cloudflare.com/tag/rust/rss)）。

---

## 6. 主流引擎横向对比表

| 引擎 | 编译后端 | Warmup 速度 | 峰值吞吐 | AOT 能力 | 嵌入式占用 | GC 支持 | SIMD | 授权 | 典型场景 |
|---|---|---|---|---|---|---|---|---|---|
| **V8** | Liftoff（baseline）+ TurboFan/Turboshaft（优化），[v8.dev Liftoff](https://v8.dev/blog/liftoff) | 极快：Liftoff 出码比 TurboFan 快约 10×，70MB 模块启动压到 1~2s（[ursb.me](https://ursb.me/immersive/webassembly/)） | 高：TurboFan 是业界参考线，Cranelift 仅比其慢约 2%（[cranelift.dev](https://cranelift.dev/)） | 不直接暴露 AOT（浏览器场景）；Snap 快照机制替代 | 大（V8 全量 ~50MB+） | WasmGC 已落地 | 完整（已 GA） | BSD-3 | Chrome/Node/Deno/Cloudflare Workers |
| **Wasmtime (Cranelift)** | Cranelift（优化，ISLE 指令选择）+ Winch（baseline）+ Pulley（解释器），[BA 2023](https://bytecodealliance.org/articles/wasmtime-and-cranelift-in-2023) | 快：Winch 单趟出码；Cranelift 比 LLVM 快但比 Winch 慢 | 高：比 V8 慢约 2%、比 LLVM 慢约 14%（[cranelift.dev](https://cranelift.dev/)） | 原生支持 `cranelift-object`，输出 .o/.so（[docs.wasmtime.dev](https://docs.wasmtime.dev/contributing-architecture.html)） | 中（Rust 运行时，数 MB 级） | WasmGC 路线图中 | 完整 | Apache-2.0 | Fastly Compute、Spin、Shopify Functions |
| **WAMR (iwasm)** | classic/fast interpreter + Fast JIT + LLVM JIT + wamrc AOT，[ESP 组件页](https://components.espressif.com/components/espressif/wasm-micro-runtime/versions/2.2.0~2) | 极快：interpreter 零编译；AOT 运行时零编译 | 高（AOT/LLVM-JIT 近原生） | 原生：wamrc 产 .aot，自实现 loader，依赖内置 mini-C runtime（[W3C PDF](https://www.w3.org/2020/08/29-chinese-web/wamr.pdf)） | 极小：VMCore 50K(AOT)/80K(interp)，hello-world 低至 16K DRAM（[W3C PDF](https://www.w3.org/2020/08/29-chinese-web/wamr.pdf)；[TWINE](https://arxiv.org/pdf/2103.15860.pdf)） | 有限/路线图 | 部分 | Apache-2.0 | ESP32/SGX/TEE/IoT/MCU |
| **Wasmer** | Singlepass（单趟）/ Cranelift / LLVM 三后端可切换，[官方文档](https://docs.wasmer.io/runtime/features/) | Singlepass 极快（qjs.wasm 102ms），Cranelift 295ms（[Wasmer 博客](https://wasmer.io/es/posts/wasmer-python-embedding-1_0)） | LLVM 后端近原生，比另两者快约 50%（[crates.io](https://crates.io/crates/wasmer/7.3.0-rc.1)） | 原生支持（`wasmer compile`） | 中 | 路线图 | 完整 | MIT | 多语言嵌入 SDK、区块链、Wasmer Edge |
| **JavaScriptCore** | IPInt 解释器 + BBQ（baseline）+ OMG（优化）+ B3 后端，[WebKit](https://webkit.org/blog/7691/webassembly/) | 快：Bun 冷启动 ~8ms vs Node ~45ms（[josenaldo](https://josenaldo.com.br/codex-technomanticus-site/03-dominios/tecnologia/20---bun-como-runtime-e-toolkit-all-in-one)） | 略低于 V8，但启动/内存更优（[Bun 文档](https://bun.com/docs/runtime)） | 不直接暴露 | 中（单文件分发） | WasmGC 跟进中（[JetStream 3](https://webkit.org/blog/17899/introducing-the-jetstream-3-benchmark-suite/)） | 完整（IPInt 已覆盖） | LGPL-2.1 | Safari、Bun、Apple 系嵌入 |
| **Wasmi** | 纯 Rust 解释器，无 JIT，[docs.rs](https://docs.rs/crate/wasmi_wast/2.0.0-beta.8) | 零编译、确定性、可跑在禁止 JIT 的环境 | 低：2.0 比旧版快 ~2.2×，与 Wasm3/Stitch 同档（[wasmi 2.0](https://wasmi-labs.github.io/blog/posts/wasmi-v2.0/)） | 不支持 | 极小（no_std 友好） | 路线图（tracking issues） | 有限 | Apache-2.0 | iOS 沙盒、合约解释执行、嵌入式 |

---

## 7. 结语

Wasm 运行时的工程分化已经非常清晰：**浏览器/通用 runtime** 走"baseline + 优化 JIT"双 tier（V8、JSC、Wasmtime）；**嵌入式/TEE** 走"解释器 + AOT"双轨（WAMR）；**serverless/边缘** 走"AOT 或快照"把编译成本前移到部署期（Fastly、Cloudflare、Shopify）；**区块链/不可信合约** 走"单趟无优化编译器"抗 JIT-bomb（Wasmer Singlepass、Wasmi）。未来 2~3 年的关键变量是 WasmGC、Component Model、Memory64 与 stack-switching 的落地成熟度——它们决定 Wasm 能否从"计算沙箱"进化为"通用多语言微进程平台"。
