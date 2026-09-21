# WebAssembly 提案现状与现代 JIT 经典技术调研

> 撰写时间：2026-09-21。本文面向"自研 Wasm 运行时"的技术选型，分两部分：(1) Wasm 各提案的阶段、语义与实现影响；(2) V8 / HotSpot / SpiderMonkey / LuaJIT / Cranelift / WAMR 的分层 JIT 经典技术。所有关键论断均附 URL，区分"已标准化"与"提案中"。

---

## 第一部分　WebAssembly 提案现状

### 0. 阶段体系与时间线背景

Wasm CG 采用 phase 0~5 流程（见 process 文档）：phase 1=提案接受、phase 2=有正式 spec 文本草案、phase 3=实现阶段、phase 4=标准化（WG 采纳）、phase 5=合并入 spec 主分支。已进入 spec 的提案见 [finished-proposals.md](https://github.com/WebAssembly/proposals/blob/main/finished-proposals.md)，活跃提案见 [README](https://github.com/WebAssembly/proposals/blob/main/README.md)。

2025-09-17 官方宣布 **Wasm 3.0 完成**，此前的 2.0 版本发布于 2025-09-16（见 [Wasm 3.0 Completed](https://webassembly.org/news/2025-09-17-wasm-3.0/) 与 [WebAssembly Spec Release 2.0 PDF](https://webassembly.github.io/custom-descriptors/versions/core/WebAssembly-2.0.pdf)）。这意味着过去被当作"实验性提案"的一大批特性，到 2026 年已经固化为正式 spec。下文阶段标注以此为准。

---

### 1. GC 提案（struct / array / i31ref / anyref）

- **Spec 链接**：[WebAssembly/gc](https://github.com/WebAssembly/gc)；核心语法见 [function-references bikeshed](https://webassembly.github.io/function-references/core/bikeshed/index.html) 与 [spec instructions](https://webassembly.github.io/spec/core/syntax/instructions.html)。
- **当前阶段**：**已标准化（Wasm 3.0，2024-07-10 进入 phase 4）**。
- **核心语义**：在原有 externref/funcref 之外引入 *heap types*：`struct`（固定字段记录，可带 mut/immut 与子类型）、`array`（定长/可变长）、`i31ref`（把 31 位小整数直接塞进指针的 tagged 表示，避免堆分配）、`anyref`（所有 GC 堆对象的公共根）。配套 `struct.new`、`struct.get/set`、`array.new`、`ref.cast`、`ref.test`、`br_on_cast` 等指令，以及一等子类型（subtyping）与递归类型组（rec group）。
- **引擎侧实现**：
  - **V8**：Chrome 119（2023-10）起默认开启 WasmGC，见 [WasmGC enabled by default in Chrome](https://developer.chrome.com/blog/wasmgc)。V8 把 WasmGC 对象接入现有 Oilpan/CppGC 堆，struct/array 字段访问走带 map 的 inline 检查，避免动态语言那种去优化。
  - **JSC / WebKit**：Safari 18.2（2024-12）支持 WasmGC，见 [WebKit Features in Safari 18.2](https://webkit.org/blog/16301/webkit-features-in-safari-18-2/)。WebKit 后来把 struct/array 从"对象头 + 间接 backing store"改为内联存储，以减少一次指针解引用，见 [JetStream 3 说明](https://webkit.org/blog/17899/introducing-the-jetstream-3-benchmark-suite/)。
- **对自研运行时的影响**：GC 提案是"最重"的一块——它把 Wasm 从"纯值+线性内存"推到了"带堆对象、类型图、GC root 枚举"的世界。**自研运行时如果只跑 C/C++/Rust 可后置；如果要接 Kotlin/Wasm、Flutter Wasm、J2CL、Blazor 等托管语言，则必须第一时间支持**，否则这些语言的 ABI 直接断掉。

---

### 2. Exception Handling（try/catch / exnref）

- **Spec 链接**：[WebAssembly/exception-handling](https://github.com/WebAssembly/exception-handling)；JS API 见 [exception-handling/js-api](https://webassembly.github.io/exception-handling/js-api/)。
- **当前阶段**：**已标准化（Wasm 3.0，2025-07-23 进入 phase 4）**。
- **核心语义**：引入 *exception tag*（带 payload 类型签名的命名标签）、`try_table` block（一条指令内列出多条 `catch $tag` / `catch_all` / `delegate` 分支）、`throw` / `throw_ref` / `catch` 指令，以及一等的 `exnref` 引用类型。异常值是带 tag 的装箱对象，跨 instance 传递。
- **对运行时的影响**：需要在 ABI 层落地 unwind 表（DWARF CFI 或 Windows EH info）、在堆上分配异常对象、并在编译器前端把 try_table 降低为真正的 landing pad。C++ 的 Itanium ABI、Rust 的 panic=unwind 都依赖它。**对自研运行时属"中期必做"**：不支持就无法跑带异常展开的 C++（除非强制 `-fno-exceptions`）。

---

### 3. Tail Call（return_call / return_call_indirect）

- **Spec 链接**：[WebAssembly/tail-call](https://github.com/WebAssembly/tail-call)。
- **当前阶段**：**已标准化（Wasm 3.0，2024-07-10 phase 4）**。
- **核心语义**：新增 `return_call`（直接尾调用）与 `return_call_indirect`（间接尾调用），语义上等价于"换栈帧后再调用"，保证栈深度不随递归增长。编译器后端必须把它 lowering 成真正的 *branch to function epilogue*（或直接 `jmp` 到被调用函数 prologue），而不是普通 `call` + `ret`。
- **对运行时的影响**：实现成本低（一条 JMP 而已），但**对栈遍历器、GC root 枚举、unwind 信息、调试器符号都有微妙影响**——你不能假设每个 call 都有一个完整栈帧。自研运行时建议**早做**，因为 OCaml、Kotlin 协程、Scheme 这类语言的尾递归正确性依赖它。

---

### 4. SIMD（fixed-width 128-bit）与 Relaxed SIMD

- **Spec 链接**：fixed-width SIMD [WebAssembly/simd](https://github.com/WebAssembly/simd)；relaxed SIMD [WebAssembly/relaxed-simd](https://github.com/WebAssembly/relaxed-simd)。
- **当前阶段**：
  - **Fixed-width 128-bit SIMD：已标准化（Wasm 2.0，2021-07 phase 4）**。
  - **Relaxed SIMD：已标准化（Wasm 3.0，2024-07 phase 4）**。
- **核心语义**：fixed-width SIMD 固定 v128 向量类型与 ~100 条语义明确的指令（add/mul/shuffle/load-lane 等）。Relaxed SIMD 引入 *non-deterministic* 指令（如 `relaxed_trunc_sat_f64x2_u_zero`、`fma` / `fnma`、relaxed 对饱和转换、laneselect），允许后端在不同 ISA（x86 AVX/SSE、ARM NEON、WASM 解释器）之间选择"最自然的"语义，但要求编译器在 AOT 时就绑定一套具体 lowerings。
- **对运行时的影响**：fixed-width SIMD 是浏览器标配，**自研运行时若面向多媒体/科学计算必须支持**；relaxed SIMD 可以后置，但一旦支持就要做"语义锁版本"——同一模块在 x86 与 ARM 上的浮点尾差可能不同。

---

### 5. Threads / Atomics / Shared Memory

- **Spec 链接**：[WebAssembly/threads](https://github.com/webassembly/threads)；MDN 概览见 [Understanding Wasm text format](https://developer.mozilla.org/en-US/docs/WebAssembly/Guides/Understanding_the_text_format)。
- **当前阶段**：**Phase 4（已被 WG 接受，截至 2026-09 仍未合并入主 spec 文本，见 [proposals README](https://github.com/WebAssembly/proposals/blob/main/README.md)）**。注意：在浏览器实践中它早已随 SharedArrayBuffer 广泛可用，但从"spec 版本号"角度它不是 2.0/3.0 已合并项。
- **核心语义**：`memory.shared` + 一组原子指令 `i32.atomic.load/store`、`atomic.rmw.add/sub/and/or/xor`、`atomic.wait`/`notify`、`memory.atomic.notify`/`wait32/64`，以及 `fence`。线程模型是"每线程独立值栈与 locals，唯一共享状态是线性内存"。
- **对运行时的影响**：需要 pthread 抽象、真正的原子 load/store（不能用普通 load 糊弄）、wait/notify 与 OS futex 对接。**自研运行时若要跑多线程 C++/Rust（rayon、tokio 多线程 runtime）就是硬需求**；单线程嵌入式场景可后置。

---

### 6. Memory64

- **Spec 链接**：[WebAssembly/memory64](https://github.com/WebAssembly/memory64)；变更记录见 [memory64/changes](https://webassembly.github.io/memory64/core/appendix/changes.html)。
- **当前阶段**：**已标准化（Wasm 3.0，2025-07-23 phase 4）**。
- **核心语义**：把线性内存的索引类型从 `i32` 升级为 `i64`，允许单块线性内存寻址超过 4 GB（理论最大 4 EB）。`memory.grow`、`load/store` 等全部指令的立即数、表索引相应变宽。
- **对运行时的影响**：代码生成器所有内存操作都要分"32-bit vs 64-bit"两套 lowering；地址计算变宽后，寄存器压力上升。**面向服务器/大模型推理（>4GB 张量）必须支持；嵌入式/前端可后置**。

---

### 7. Function References 与 call_ref / call_indirect 的 typed 化

- **Spec 链接**：[WebAssembly/function-references](https://github.com/WebAssembly/function-references)；语法见 [spec instructions](https://webassembly.github.io/spec/core/syntax/instructions.html) 中 `call_ref` 一节。
- **当前阶段**：**已标准化（Wasm 3.0，2024-07-10 phase 4）**。
- **核心语义**：`funcref` 从"无类型联合"细化为 *typed function reference*（`(ref $ty)`），新增 `ref.func $f`、`call_ref $ty`。`call_indirect` 也带上显式类型立即数。类型检查在 validation 期完成，调用点不再需要运行时类型 guard（`call_ref` 不抛类型错）。
- **演进方向**：typed 化让 JIT 可以直接把 `call_ref` lower 成 *单态直接调用*（无表查表、无类型 guard），是后续 wasm 内联、去虚化、PGO 的基础。**自研运行时建议与 GC 一起做**——它们共用 rec group 与子类型基础设施。

---

### 8. Shared-Everything Threads（多线程新方向）

- **Spec 链接**：[WebAssembly/shared-everything-threads](https://github.com/WebAssembly/shared-everything-threads)；安全分析见 [Security Implications of Wasm Shared-Everything Threads](https://www.systemshardening.com/articles/wasm/wasm-shared-everything-threads-security/)。
- **当前阶段**：**Phase 1（提案阶段）**。
- **核心语义**：把"线程只能共享线性内存"扩展为"线程之间可以共享 GC 堆上的 struct/array 引用"，并引入 `shared` heap type、跨线程消息传递、共享可变数据结构。
- **对运行时的影响**：这是真正的"下一代并发模型"，会要求 GC 写屏障、跨线程指针屏障、并发标记/复制。**自研运行时不要现在做，但要在 GC 设计时预留接口**（例如所有对象指针都假设可能跨线程访问），否则以后会推倒重来。

---

### 9. Multimemory / ESM 集成 / Component Model

- **Multimemory（Multiple Memories）**：[WebAssembly/multi-memory](https://github.com/WebAssembly/multi-memory)，**已标准化（Wasm 3.0，2024-07-10）**。一个 module 可声明多块 linear memory，每条 load/store 带 memory index。对自研运行时：实现成本中等（在 ABI 里加一个 memory id 字段），**中期做**。
- **ECMAScript Module Integration（ESM 集成）**：[WebAssembly/esm-integration](https://github.com/WebAssembly/esm-integration)，**Phase 3**。目标是让 Wasm 模块能直接 `import` / `export` 成 ES module，不需要 JS 胶水。纯运行时层面影响小，主要在 embedder/loader。
- **Component Model**：[WebAssembly/component-model](https://github.com/WebAssembly/component-model)，**Phase 1**。引入 *interface types*（`string`、`record`、`variant`、`list` 等高级类型），把 Wasm 从"汇编级"升级到"组件级 ABI"，跨语言互操作（wit）。W3C 章程已把 Component Model 列为 WG 目标交付物，见 [Wasm WG Charter draft](http://w3c.github.io/charter-drafts/2026/wasm-wg-charter.html)。**自研运行时可以先做"core wasm"，组件层后期外挂**——它本质是一层 ABI 包装，不必塞进 core 编译器。

---

### 10. 其他活跃提案速查

| 提案 | 阶段 | 自研运行时优先级 |
|---|---|---|
| [Stack Switching](https://github.com/WebAssembly/stack-switching)（协程/fiber） | Phase 3 | 中（异步运行时需要） |
| [Custom Page Sizes](https://github.com/WebAssembly/custom-page-sizes) | Phase 3 | 低 |
| [Wide Arithmetic](https://github.com/WebAssembly/wide-arithmetic)（i128） | Phase 4 | 低 |
| [Threads](https://github.com/webassembly/threads) | Phase 4 | 高（多线程刚需） |
| [Acquire-Release Atomics](https://github.com/WebAssembly/acquire-release-atomics) | Phase 2 | 后置 |
| [FP16](https://github.com/WebAssembly/half-precision) | Phase 2 | 后置 |
| [Compilation Hints](https://github.com/WebAssembly/compilation-hints) | Phase 2 | 中（warmup 友好） |
| [JS Promise Integration](https://github.com/WebAssembly/js-promise-integration) | Phase 5（已合并） | embedder 层 |

### 11. 自研运行时的"先做 / 后置"清单

- **必须先做**：MVP + 2.0 已合并项（reference types、bulk memory、fixed-width SIMD、multi-value、sign extension、non-trapping conversions）、typed function references、tail call、memory64、atomics/shared memory。
- **第二批**：GC、Exception Handling、Multimemory、Stack Switching。
- **后置/外挂**：Relaxed SIMD（先固定一种 lowering）、Component Model、Shared-Everything Threads、FP16、Wide Arithmetic。

---

## 第二部分　现代 JIT 与 Trace 编译的经典技术

### 1. V8：Ignition + Sparkplug + Maglev + Turbofan/Turboshaft

V8 从 2017 年起废弃了 Full-codegen 与 Crankshaft，改为 Ignition（字节码解释器）+ TurboFan（优化 JIT）的双层结构，见 [V8: Behind the Scenes (Feb 2017)](https://benediktmeurer.de/2017/03/01/v8-behind-the-scenes-february-edition/)。2021 年加 Sparkplug（baseline 机器码），2023 年 Chrome 117 引入 **Maglev** 中档优化层，见 [Maglev 介绍](https://v8.dev/blog/maglev)（V8 官方博客）与 [V8 multi-tier pipeline 综述](https://readoss.com/en/v8/v8/v8-multi-tier-compilation-pipeline)。

- **分层与触发**：
  1. **Ignition**：解析 AST → 字节码，解释执行，同时收集 feedback。
  2. **Sparkplug**：把字节码一次性转成"无优化机器码"，消除解释器 dispatch 开销，编译时间 ~100µs。
  3. **Maglev**：基于 feedback 的中档 SSA 编译器，编译 ~1ms，代码比 Sparkplug 快 ~2×。触发点是函数调用次数累计到 ~500 且 feedback 稳定（见 [V8 Engine Architecture](https://sujeet.pro/articles/v8-engine-architecture)）。
  4. **TurboFan / Turboshaft**：顶级优化层，编译 10~100ms，做 aggressive inlining、escape analysis、load elimination、GNU-SOI 风格的 sea-of-nodes 优化。Turboshaft 是新一代 sea-of-nodes IR，正在替换 TurboFan 的中间表示。
- **Feedback Vector 机制**：每个 JS 函数在编译时附带一个 feedback vector，按字节码偏移存放 *slot*——call target、property 形状（map/feedback cell）、分支 edge counter、数组元素种类（PACKED_SMI_ELEMENTS 等）。解释器在执行时写 slot，JIT 编译时读 slot 做 spec 化。Feedback 改变会 invalidate 已优化代码。
- **字节码 → SSA**：MaglevGraphBuilder 从字节码线性扫描，按支配树构造 SSA graph，load/property 访问直接用 feedback slot 做 guard。Turboshaft 走更复杂的 sea-of-nodes，把控制流与数据流压平成图。
- **内联策略**：按"字节码大小预算"+"反馈热度"决定；monomorphic call 必内联，polymorphic（2~4 种形状）生成线性检查链，megamorphic 放弃。
- **教训**：Maglev 上线后出过 CVE-2023-4069（对象初始化不完整导致 RCE），见 [GitHub Security Blog](https://github.blog/2023-10-17-getting-rce-in-chrome-with-incomplete-object-initialization-in-the-maglev-compiler/)——说明**中档优化层最容易出安全 bug**，因为它既要快又要 spec 化。

### 2. Oracle HotSpot C1 / C2

- **分层编译（TieredCompilation，Java 7 引入）**：见 [Oracle Java HotSpot Performance Enhancements](http://docs.oracle.com/en/java/javase/11/vm/java-hotspot-virtual-machine-performance-enhancements.html)。共 5 个 tier：解释器（C0）→ C1 无 profiling → C1 有限 profiling（仅 invocation & backedge counter）→ C1 全 profiling → C2。C1 代码比解释器快但带插桩，C2 是 AOT 级别优化。
- **Profile 采集**：解释器与 C1 在方法入口、循环回边、类型转换点插 counter；C2 启动时拉这份 profile 做类型特化、虚方法去虚化（CHA + 单态内联）。
- **OSR（On-Stack Replacement）**：长循环不退出方法时，C2 编译完成后把当前栈帧"原地替换"成优化版本，见 [Tiered Compilation 综述](https://geekworkbench.com/blog/technical/tiered-compilation)。
- **Escape Analysis / 标量替换 / 锁消除**：C2 在线上 EA，把不逃逸对象拆成标量字段（scalar replacement），同步消除（lock elision），见 [HotSpot Escape Analysis Status](https://cr.openjdk.org/~cslucas/escape-analysis/EscapeAnalysis.html)。
- **Safepoint 设计**：所有可能去优化、GC 操作的点都是 safepoint；线程在 safepoint 轮询一个安全点计数器；GC 用 safepoint 枚举所有栈上的 Oop（GC root）。去优化时按 debug info 重建解释器帧，见 [Red Hat: How JIT boosts Java performance](https://developers.redhat.com/articles/2021/06/23/how-jit-compiler-boosts-java-performance-openjdk)。
- **AOT**：JEP 544 提供现代 AOT 代码缓存，见 [JEP 544](http://openjdk.org/jeps/544)。

### 3. Mozilla SpiderMonkey：IonMonkey → WarpMonkey

- **历史**：JägerMonkey（method JIT）→ IonMonkey（MIR/LIR Sea-of-Nodes）→ WarpMonkey（Firefox 83, 2020-11）。Warp 不是新 IR，而是**新的优化层与反馈机制**：用 **CacheIR**（一种线性、可重放的 stub 字节码）取代原来的 TI（Type Inference）。见 [Warp: Improved JS performance in Firefox 83](https://hacks.mozilla.org/2020/11/warp-improved-js-performance-in-firefox-83/)。
- **CacheIR 思路**：Baseline Interpreter 与 Baseline JIT 在每个 IC（inline cache）点编译一段"小 stub"，stub 本身就是 CacheIR 字节码。WarpBuilder 直接消费这些 CacheIR 记录——它不需要重新做类型推断，只要把 baseline 已经验证过的 guard 内联进优化图。
- **分层与触发**：四层（Interpreter / Baseline JIT / Warp），warmup counter 在函数入口与循环头递增，**默认 1500 次进入 Warp**，见 [How SpiderMonkey Optimizes](https://firefox-source-docs.mozilla.org/js/how-we-optimize.html)。
- **对自研的启发**：把"反馈"做成可序列化、可重放的 stub 字节码，比 V8 那种"feedback slot + 解释器插桩"更工程友好——WarpBuilder 几乎只是"读 CacheIR 生成 MIR"。

### 4. LuaJIT：Trace-based JIT

- **核心机制**：不编译整个函数，而是在解释执行时*记录*热点线性路径（trace），编译成机器码。主入口 `lj_record_ins()` 把每条字节码翻译成 IR 节点；hotloop 默认 **56 次迭代**、hotexit **10 次**触发 side trace，见 [LuaJIT Running](https://luajit.org/running.html)。
- **mcode cache**：所有 trace 的机器码放在一块可执行内存里（默认 32MB），trace 之间用 *link* 指针衔接（tail-call 风格拼 trace）。
- **Snapshot**：每条 side exit 配一份 snapshot，记录退出时 GPR/FPR/栈槽的位置，用于退回解释器。默认 maxsnap=500，见 [LuaJIT Running](https://luajit.org/running.html)。
- **NYI 边界**：解释器遇到无法 trace 的字节码（如某些 coroutine、`next()` 早期版本）就 abort 这条 trace；LuaJIT 2.1 引入 trace stitching 让 NYI 后能接着记录，见 [Cloudflare: getting next() out of NYI](https://blog.cloudflare.com/luajit-hacking-getting-next-out-of-the-nyi-list/)。
- **对自研的启发**：trace JIT 对循环密集代码（数值计算、游戏内循环）极快，但对"函数调用密集 + 多态"代码不如 method JIT。Wasm 本身已经是结构化字节码，trace 化成本低，但**多态 call_ref 是 trace 的天敌**。

### 5. Self / Crankshaft 的历史教训

- **Self（Urs Hölzle 1994）**：第一个把"自适应优化 + 去优化（deoptimization）"工程化的系统，发明了 polymorphic inline cache、type feedback、optimized code deopt 回退解释器的整套范式。
- **Crankshaft（V8 2010–2017）**：采用线性扫描寄存器分配、激进内联、基于反馈的类型特化，见 [Crankshaft 概述](http://nothingcosmos.github.io/V8Crankshaft/src/blog.html)。**教训**：(a) 它把"类型假设"硬编码在大量特殊代码路径里，每加一个 JS 新特性就要改编译器；(b) 跨编译器（full-codegen vs Crankshaft）的 IR 不一致导致维护成本爆炸；(c) 2017 年 V8 团队在 [V8: Behind the Scenes](https://benediktmeurer.de/2017/03/01/v8-behind-the-scenes-february-edition/) 中宣布用 Ignition + TurboFan 全面替换——理由是"旧 IR 跟不上新语言特性"。
- **对自研的教训**：**从第一天起就用统一的 SSA/sea-of-nodes IR**，不要为了快而做两套 IR；speculative optimization 必须配 deopt，但 deopt 的"重建解释器帧"要在 IR 层就设计好。

### 6. 通用 JIT 工程要点

- **字节码 → SSA**：构造 CFG，按支配树插入 φ；用活跃变量分析决定 φ 需求。
- **寄存器分配**：线性扫描（linear scan）快、简单，Crankshaft/C1 用；图着色（graph coloring）质量高、慢，C2/TurboFan 用。Cranelift 2024 重写了新寄存器分配器，见 [Cranelift 官方](https://cranelift.dev/) 与 [LWN: Cranelift](https://lwn.net/Articles/965369/)。
- **Instruction Selection / LIR / Code Emission**：把 SSA lowering 成机器无关 LIR，再做 ISA 选择（pattern matching / DSL 元编译），最后 emit。Cranelift 用 ISLE DSL 做 instruction selection。
- **Code Patching**：JIT 出来的代码必须在原地可改——inline cache 把"未初始化 stub"patch 成 monomorphic stub，再 patch 成 megamorphic；deopt guard 点 patch 回解释器入口。
- **去优化（deoptimization）**：每个 guard 点配一份"描述符"，记录如何把优化帧还原成解释器帧（栈槽映射、常量重放）。
- **Safepoint 与 GC root 枚举**：所有可能发生 GC 的点（如 call、分配、safepoint poll）都要能枚举栈/寄存器上的 GC 指针。WasmGC 时代这一点从"可选"变成"必选"。
- **Profile 采集**：edge counter（分支偏向）、type feedback（对象形状）、call counter（热度）。Wasm 是静态类型语言，type feedback 用途弱，但 edge counter 与 call counter 对分支预测、内联仍有价值。
- **PGO / AOT 数据流**：线上跑一遍采样，把 edge counts 回灌到 AOT 编译器，做 layout、outline、branch predication。OpenJDK JEP 544 是现代范例，见 [JEP 544](http://openjdk.org/jeps/544)。

---

### 7. 对照表：JIT 技术点 × 引擎实现位置

| JIT 技术点 | V8 | HotSpot | LuaJIT | Cranelift / Wasmtime | WAMR | 对自研 Wasm 运行时的可借鉴度 |
|---|---|---|---|---|---|---|
| 分层 tier-up | Ignition→Sparkplug→Maglev→TurboFan | C0→C1→C2 | trace 记录即 tier-up | Cranelift（优化）+ Winch（baseline 单次扫描） | classic→fast interp→Fast JIT→LLVM JIT | **高**：WAMR 的"解释器→Fast JIT→LLVM JIT"是最贴近 Wasm 的范本 |
| Feedback vector / IC | feedback slot + Map | 解释器/C1 插桩 | 无（trace 自带） | 无（静态类型，靠 PGO） | 无 | **中**：Wasm 静态类型，IC 价值低，但 edge counter 仍有用 |
| 字节码→SSA | Maglev/TurboFan SSA | C2 IR（sea-of-nodes）| IR 节点（trace 内）| CLIF（Rust） | LLVM IR 直出 | **高**：必做 |
| 内联 | 字节码预算 + monomorphic | CHA + 单态内联 | trace 内联 | 2025 年新增 function inliner，见 [BA: inliner](https://bytecodealliance.org/articles/inliner) | LLVM 默认内联 | **高**：Wasm 已经是 IR，内联性价比极高 |
| 去优化 | deopt guard | deopt + 重建帧 | snapshot 退回解释器 | 无（不做 spec 化） | 无 | **中**：WasmGC 引入类型 cast 后才需要 |
| Safepoint / GC root | 内置 Oilpan GC | safepoint poll + oop map | 极简化 | 由 embedder 提供 | 由 embedder 提供 | **高**：做 GC 提案前必须先设计 |
| 寄存器分配 | TurboFan 图着色 | C2 图着色 | 线性扫描 | 2024 重写新分配器 | LLVM | **高**：初期用线性扫描，后期换图着色 |
| Trace 编译 | 否 | 否 | 是 | 否 | 否 | **低-中**：仅当面向数值/循环负载 |
| AOT + PGO | 无（仅 JIT） | JEP 544 AOT cache | 无 | Wasmtime 支持 AOT | wamrc AOT | **高**：服务器场景必做 |

---

### 8. Warmup 工程：从冷启动到稳态

缩短 warmup 的本质是"在编译成本与执行速度之间找最优调度"。各引擎手段：

- **V8**：
  - LazyParse：函数体首次被调用才解析。
  - 字节码缓存（code stub cache）：把解释器字节码与 baseline 机器码持久化到磁盘（`.isolate`/`code_cache`）。
  - Sparkplug 异步后台编译：函数还在解释执行时，后台线程先把它编成 baseline 机器码，下次进入即受益。
  - Maglev tier-up 阈值 ~500 次调用 + feedback 稳定；feedback 抖动时不升级。
- **HotSpot**：
  - TieredCompilation 让 C1 边跑边采 profile，避免"纯解释期"慢。
  - OSR：长循环不退出也能升级到 C2。
  - AOT cache（JEP 544）：把上次跑过的 profile 与编译结果持久化，下次启动直接用。
  - `AppCDS`：把核心类元数据预加载。
- **SpiderMonkey**：
  - Baseline Interpreter 同时充当"快速解释 + feedback 采集器"，warmup counter 到 1500 才进 Warp。
  - CacheIR stub 是增量式的：每多一种对象形状就追加一条 stub，避免全量重编译。
- **LuaJIT**：
  - 无"分层"概念——一旦 hotloop 阈值到了就立刻 trace 并编译，warmup 极短；代价是 trace 编译本身要花 ~ms。
  - 关闭 jit 时纯解释，开启后第一次热循环就要付编译成本。
- **Cranelift / Wasmtime**：
  - 设计哲学是"编译快过解释"，因为 Wasm 是静态类型，没有 spec 化空间。Winch（单次扫描 baseline）作为冷启动快路径，Cranelift 作为稳态优化，见 [Wasmtime stability tiers](https://docs.wasmtime.dev/stability-tiers.html)。
  - Wasmtime 全程 AOT 友好：`wasmtime compile` 把模块预编译成 .cwasm，启动零编译成本。
- **WAMR**：
  - 三档：classic interpreter（兼容但慢）→ fast interpreter（直接 token-threaded）→ Fast JIT（轻量一次性机器码）→ LLVM JIT（优化后机器码），并支持**从 Fast JIT 动态 tier-up 到 LLVM JIT**，见 [WAMR 介绍](https://github.com/bytecodealliance/wasm-micro-runtime) 与 [ESP Component Registry](https://components.espressif.com/components/espressif/wasm-micro-runtime/versions/2.2.0~2)。
  - wamrc AOT：把整个 wasm 模块离线编译成 native .aot，运行时零 JIT。

**对自研运行时的建议**：
1. 冷启动路径：一个 token-threaded 解释器（~fast interpreter 量级）+ 单次扫描 baseline JIT（Winch/WAMR Fast JIT 风格），保证启动 < 50ms。
2. 稳态路径：一个 per-function 优化编译器（Cranelift 风格，SSA + 简单寄存器分配 + 内联）。
3. tier-up 触发：基于 call counter + 循环 backedge counter，阈值参考 HotSpot（~10k invocations）与 V8（~500 for Maglev，因为 V8 的函数调用远比 JVM 频繁）。
4. 持久化：code cache + PGO 回灌是服务器场景的必备，参考 JEP 544 与 Wasmtime AOT。

---

## 附：全部引用 URL（去重）

1. https://github.com/WebAssembly/proposals/blob/main/finished-proposals.md
2. https://github.com/WebAssembly/proposals/blob/main/README.md
3. https://webassembly.org/news/2025-09-17-wasm-3.0/
4. https://webassembly.github.io/custom-descriptors/versions/core/WebAssembly-2.0.pdf
5. https://webassembly.github.io/spec/core/syntax/instructions.html
6. https://webassembly.github.io/function-references/core/bikeshed/index.html
7. https://webassembly.github.io/memory64/core/appendix/changes.html
8. https://webassembly.github.io/exception-handling/js-api/
9. https://github.com/WebAssembly/gc
10. https://github.com/WebAssembly/exception-handling
11. https://github.com/WebAssembly/tail-call
12. https://github.com/WebAssembly/simd
13. https://github.com/WebAssembly/relaxed-simd
14. https://github.com/webassembly/threads
15. https://github.com/WebAssembly/memory64
16. https://github.com/WebAssembly/function-references
17. https://github.com/WebAssembly/shared-everything-threads
18. https://github.com/WebAssembly/multi-memory
19. https://github.com/WebAssembly/esm-integration
20. https://github.com/WebAssembly/component-model
21. https://github.com/WebAssembly/stack-switching
22. https://github.com/WebAssembly/custom-page-sizes
23. https://github.com/WebAssembly/wide-arithmetic
24. https://github.com/WebAssembly/acquire-release-atomics
25. https://github.com/WebAssembly/half-precision
26. https://github.com/WebAssembly/compilation-hints
27. https://github.com/WebAssembly/js-promise-integration
28. http://w3c.github.io/charter-drafts/2026/wasm-wg-charter.html
29. https://developer.mozilla.org/en-US/docs/WebAssembly/Guides/Understanding_the_text_format
30. https://www.systemshardening.com/articles/wasm/wasm-shared-everything-threads-security/
31. https://developer.chrome.com/blog/wasmgc
32. https://webkit.org/blog/16301/webkit-features-in-safari-18-2/
33. https://webkit.org/blog/17899/introducing-the-jetstream-3-benchmark-suite/
34. https://v8.dev/blog/maglev
35. https://readoss.com/en/v8/v8/v8-multi-tier-compilation-pipeline
36. https://sujeet.pro/articles/v8-engine-architecture
37. https://benediktmeurer.de/2017/03/01/v8-behind-the-scenes-february-edition/
38. https://github.blog/2023-10-17-getting-rce-in-chrome-with-incomplete-object-initialization-in-the-maglev-compiler/
39. http://docs.oracle.com/en/java/javase/11/vm/java-hotspot-virtual-machine-performance-enhancements.html
40. https://cr.openjdk.org/~cslucas/escape-analysis/EscapeAnalysis.html
41. https://developers.redhat.com/articles/2021/06/23/how-jit-compiler-boosts-java-performance-openjdk
42. https://geekworkbench.com/blog/technical/tiered-compilation
43. http://openjdk.org/jeps/544
44. https://hacks.mozilla.org/2020/11/warp-improved-js-performance-in-firefox-83/
45. https://firefox-source-docs.mozilla.org/js/how-we-optimize.html
46. https://luajit.org/running.html
47. https://blog.cloudflare.com/luajit-hacking-getting-next-out-of-the-nyi-list/
48. http://nothingcosmos.github.io/V8Crankshaft/src/blog.html
49. https://cranelift.dev/
50. https://lwn.net/Articles/965369/
51. https://bytecodealliance.org/articles/inliner
52. https://docs.wasmtime.dev/stability-tiers.html
53. https://github.com/bytecodealliance/wasm-micro-runtime
54. https://components.espressif.com/components/espressif/wasm-micro-runtime/versions/2.2.0~2
55. https://arxiv.org/pdf/2205.01183v1.pdf
