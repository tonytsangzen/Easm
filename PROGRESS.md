# esam (embed WebAssembly) 运行时 — 阶段性进展报告

> 日期：2026-09-21 · 平台：Apple Silicon (aarch64, macOS) · 目标：1:1 指令级翻译的 baseline JIT + 解释器 fallback

## 一、当前总体状态

| 里程碑 | 状态 |
|---|---|
| 二进制解码器（LEB128 / 全 section / Wasm 2.0+3.0 特性位） | ✅ |
| 校验器（类型栈机 + ctrl 帧 + unreachable 多态） | ✅ |
| 运行时（实例化 / 常量表达式 / import 链接 / 12GiB 保留内存） | ✅ |
| 解释器（值栈 + RCtl 控制帧 + longjmp trap） | ✅ |
| aarch64 baseline JIT（1:1 降级翻译，不可降级指令自动回退解释器） | ✅ |
| 一致性覆盖率（官方 spec 测试套） | ✅ 170/258 文件全过（解释器与 JIT 模式完全一致） |
| Benchmark 对比（wasmtime / node） | ✅ JIT 较解释器 17–31×，fib/primes 快于 node |

可执行文件 `build/easm` 仅 **188 KB**（目标 <200KB ✅）。

## 二、一致性覆盖率（官方 spec 套件）

运行方式：`python3 tools/run_spec.py -j 8 [--jit]`（内部 wast2json + 并行 + 缓存）

- **258** 个 wast 文件中 **170 个完全通过（65.9%）**，累计通过 **57,483** 条 assert。
- `--jit` 与解释器模式**逐文件结果完全一致**：JIT 编译成功的函数真正执行机器码，
  不能降级的函数（v128 / br_table 等）自动回退解释器，正确性对齐。
- 25 个文件为 wast2json 本身转换失败（function-references/GC 新语法，与运行时无关）。
- 剩余 63 个失败文件的主因（按规划属二阶段特性）：
  - function-references / GC（`ref.func` 类型化、`call_ref`、struct/array）
  - exception-handling（try/catch/exn）
  - tail-call、relaxed-simd、custom-page-sizes、multi-memory / memory64 边角
  - 少量校验器严格性缺口（如 `align.wast` 中 4 条 assert_invalid / assert_malformed）

## 三、Benchmark（best-of-2，单位秒，越低越好）

内核：`bench/bench.c`（freestanding wasm32，clang -O2）

| kernel | easm-jit | easm-int | wasmtime | node | JIT/解释器加速 | JIT vs node |
|---|---|---|---|---|---|---|
| fib (10M 迭代) | **0.026** | 0.448 | 0.009 | 0.032 | **17.2×** | 快 1.2× |
| primes (500K 筛) | **0.014** | 0.312 | 0.006 | 0.029 | **22.3×** | 快 2.1× |
| sum (100M i64) | **0.356** | 9.160 | 0.021 | 0.081 | **25.7×** | 慢 4.4× |
| matmul (30×128³) | **0.581** | 17.844 | 0.031 | 0.096 | **30.7×** | 慢 6.1× |
| memsum (2M load) | **0.003** | 0.008 | 0.005 | 0.028 | 2.7× | 快 |

结论：
1. 1:1 栈机翻译的 baseline JIT 相对解释器获得 **17–31×** 加速（memsum 除外——它本身是内存带宽瓶颈）。
2. 在分支/调用密集的整数内核（fib、primes）上 **已快于 node**（Liftoff+TurboFan 混合）。
3. 相比 wasmtime（Cranelift 寄存器分配 + LLVM 后端）仍有 2–19× 差距 —— 这正是规划中
   HIR / 优化编译器阶段的收益空间（寄存器分配、冗余栈操作消除、SIMD 原生 lowering）。

## 四、本次修复的 JIT 缺陷（按发现顺序）

1. **比较条件码整体错位**：`CC_HI/LS/GE/LT/GT/LE` 常量 +2（如 `CC_LE=15` 实为 NV），
   导致 le/ge/lt/gt 分支与 cset 全部错条件；(1,2) 这类测试值恰好掩盖了 lt/le 差异。
2. **i32/i64.le_s / le_u 交换操作数**：`cmp b, a + LE` 计算的是 `b<=a`。
3. **变量移位编码器错位**：`lslv/lsrv/rorv` 常量误含 bit19（Rm 第 3 位），移位量实际读自 x25。
4. **浮点运算编码大面积错误**（对照 llvm-mc 逐一修正）：
   fadd/fsub/fmul/fmax/fmin 的 opcode 常量错位（fmul 实际执行成非法指令→崩溃）、
   fneg/frintp/frintm/frintz 错位、`fmov Wd, Sn` 32 位基址错、
   `scvtf/ucvtf` 混入移位形式残留位、**FCMP 误置 Rd=31（保留位，触发非法指令）**。
5. **调用约定 ABI 三处不一致**（顶链 trampoline / JIT→JIT / JIT→解释器桥）：
   - callee 序言 `stp x29,x30,[sp,#-32]!` 会覆盖入口参数区（A≥2 必坏，A=1 侥幸）；
     改为「先跳过参数区再建帧」，fp = entry − A·16 − R·16 − 32；
   - R>A 时结果区与自身帧冲突：序言额外预留 (R−A)·16；
   - callee 返回约定统一为 `sp = entry − R·16`、结果置于栈顶 R 槽；
   - JIT→JIT 调用路径补上「跳过解释器桥」的无条件分支（此前会双执行 + sp 漂移）；
   - 参数/结果槽序修正：arg_i = 第 (i+1) 个入口下槽（深端在高地址）。
6. **memory.fill/copy/init、table.* 等 helper 操作数序颠倒**：栈顶是 n（最后压入），
   此前按 [d,mid,n] 读取实为 [n,mid,d]。
7. **emit_store 大静态偏移（>4095）覆盖暂存值寄存器 R17**：store 写出去的值变成偏移量
   （正是 primes(500K) +1 的根因：clang 合并条件字节 store 后使用大偏移）。
8. **imm9 编码器静默截断**：负偏移 < −256 时 LDUR/STUR 的 imm9 溢出静默环绕 → 实例指针
   读到 0 → SIGBUS；所有 imm 访存编码器补上「大偏移物化」回退路径。
9. **函数级 `br`（br N 跳出函数）读 ctrl[-1] 越界**：补函数帧特例（arity=R，目标=隐式 end）；
   以及 unreachable 到函数尾时 `ctrl[csp-1]` 的 csp==0 崩溃。
10. **编码器正确性回归保障**：新增 `emit_test.c` 63 条编码用例对照 llvm-mc，
    现为 63/63 全对（含 SP 读写、条件码、移位、浮点全套）。

## 五、优化迭代记录

### 迭代 1：浮点操作数直通 S/D 寄存器（已完成，2026-09-21）

改动（`src/a64_emit.h` + `src/jit_a64.c`）：
- 新增 FP 寄存器访存编码器（LDR/STR S·D 的 post/pre/scaled/寄存器偏移五种形式，
  全部经 llvm-mc 逐条比对验证），以及 `pop_s/pop_d/push_s/push_d` 栈槽原语；
- f32/f64 二元运算：7 条指令 → **4 条**（`ldr s1; ldr s0; fadd s0,s0,s1; str s0`），
  彻底去掉 fmov gpr↔fpr 往返；
- 一元运算（abs/neg/sqrt/ceil/floor/trunc/nearest）：5 条 → **3 条**；
- fcmp：7 条 → 4 条；f32/f64 的 memory load/store 直接在 V 寄存器与线性内存之间搬运
  （顺带消除 float store 中间的 R0 搬移序列）；
- f32/f64 min/max 保留 C helper 调用（ARM FMAX/FMIN 的 NaN 结果不保证 canonical，
  与 wasm 语义有差，正确性优先）。

验证：
- flt/fltmem 全部用例（双参数四则、min/max、NaN 比较、sqrt/nearest、
  f32/f64 内存往返、load+add 混合）JIT 与解释器逐值一致；
- 全量 spec 套件 --jit 模式重跑：仍为 170/258，失败文件集与优化前完全相同（零回归）；
- 5 个 bench 内核 JIT 与解释器输出逐值一致。

Benchmark 前后（秒，best-of-2）：

| kernel | 优化前 jit | 优化后 jit | 变化 |
|---|---|---|---|
| matmul | 0.581 | 0.563 | **+3.2%** |
| sum | 0.356 | 0.355 | 持平（纯整数内核，无浮点，符合预期） |
| fib / primes / memsum | 0.026 / 0.014 / 0.003 | 0.025 / 0.014 / 0.003 | 持平（无浮点） |

结论：fmov 往返消除在浮点密集的 matmul 上收益 +3.2%，符合"1:1 翻译下 fmov 本身很廉价"
的判断——matmul 热循环的大头是每次访存的边界检查与地址算术。代码生成更干净、
栈槽流量更小，为后续寄存器分配阶段打好基础。下一个大幅提升 matmul/sum 的杠杆是
HIR + 寄存器分配（消除冗余 push/pop 与循环内重复边界检查）。

### 迭代 2：延迟操作数融合 + emit_br_to 槽位修复（已完成，2026-09-21）

改动：
- **延迟操作数融合**（`i32/i64.const`、`local.get` 把值驻留 x17，紧随其后的二元/比较
  指令直接从寄存器取第二个操作数，省掉 str+ldr 一对访存）：
  - 安全性：仅当预扫描确认「无任何分支可落到消费者指令」时才延迟（block end / loop 头 /
    else 落点 / 函数尾全部标记为 target）；其余一切指令在派发时先冲刷；
  - 覆盖 38 条消费指令（i32/i64 的 add/sub/mul/and/or/xor/shl/shr_s/shr_u 与全部比较）；
- **顺带修复 `emit_br_to` 的槽位计算错误**（此前就存在）：源/目的把绝对槽号当作
  sp 相对偏移、弹出量少加 arity——只有 arity=0 或 cur==target 的用例侥幸正确
  （如 `block (result i32) (i32.const 7) (br_if 0 …)` 携值跳出会返回垃圾）。

验证：fuse 边界用例（块尾 const+br_if 携值、循环头、负数比较、if+const）全部 JIT==int；
全量 spec 套件 --jit 仍 170/258，失败集不变（零回归）；5 个内核数值逐值一致。

Benchmark 前后（秒，best-of-2，对比迭代 1 后）：

| kernel | 迭代1后 | 迭代2后 | 提升 | 两轮累计 |
|---|---|---|---|---|
| sum | 0.355 | **0.261** | **+26%** | 0.356 → 0.261（+27%） |
| matmul | 0.563 | **0.466** | **+17%** | 0.581 → 0.466（+20%） |
| primes | 0.014 | **0.011** | +21% | 0.014 → 0.011 |
| fib / memsum | 0.025 / 0.003 | 0.025 / 0.003 | 持平 | — |

与 wasmtime 差距：sum 17× → 12.4×，matmul 19× → 15.5×。剩余差距主要来自
LLVM 后端的循环展开/NEON 向量化与完整寄存器分配，属 HIR 阶段目标。

### 迭代 3：双值延迟窗口（已完成，2026-09-21）

在迭代 2 基础上把延迟窗口从 1 值扩到 2 值：`local.get/const; local.get/const; 二元指令`
两个操作数全程驻留 x16/x17，二元指令**零访存取数**（纯寄存器运算 + 一次结果压栈，
如 `local.get a; local.get b; i32.add` 从 8 条指令降到 4 条）。

关键栈语义：延迟值占用 sp 当前指向但**尚未写入**的槽——因此双值冲刷是按序两次
push（深值 R16 先），双值融合的消费指令结果是普通 push（正好落进第一个延迟值的槽位），
sp/depth 账目自洽。安全性：2 值窗口要求连续两个前瞻指令都非跳转目标且第二个是
消费指令，否则任意一环失败即在派发时按序冲刷。

验证：融合边界用例全对；全量 spec 套件 --jit 仍 170/258 失败集不变（零回归）；
5 内核基准规模输出逐值一致。

| kernel | 迭代2后 | 迭代3后 | 提升 | vs 优化基线累计 | vs wasmtime |
|---|---|---|---|---|---|
| sum | 0.261 | **0.189** | **+28%** | **+47%** | 17× → 9.5× |
| matmul | 0.466 | **0.358** | **+23%** | **+38%** | 19× → 12× |
| primes | 0.011 | **0.009** | +18% | **+36%** | 2.3× → 1.5× |
| fib | 0.025 | **0.022** | +12% | +15% | 2.9× → 2.75×（本就快于 node） |
| memsum | 0.003 | 0.003 | 持平 | 持平 | 快于 wasmtime |

与 node 对比：fib 0.022 vs 0.028、primes 0.009 vs 0.030（快 3.3×）；
sum/matmul 仍慢 2.4×/4×（node 有 LLVM 向量化）。

### 迭代 4：br_table JIT lowering（已完成，2026-09-21）

消除最后一个成规模的整数指令 fallback：此前任何含 switch 的模块（C 编译产物几乎必含）
整体回退解释器。实现为比较链式分发：索引出栈后对每个非默认标签 cmp+b.eq，
链尾无条件跳默认 case；每个 case 独立做 emit_br_to 栈修正后跳目标——天然支持
各标签不同 arity/嵌套高度，函数级标签复用函数返回路径。≥4096 的标签序号自动
切换寄存器比较（imm12 限制）。

验证：自建 5 组形态用例（嵌套标签 switch、携值、循环回边、函数级标签、零非默认标签）
全部 JIT==int；官方 br_table.json 在 JIT 与解释器模式下结果完全一致（该文件自身的
wast 驱动模块注册限制为两模式共有，与本实现无关）；全量 spec 套件仍 170/258
失败集不变；bench.wasm 的 `run` switch 分发器现在零 bail 完整 JIT 且结果正确。

Benchmark 持平（br_table 不在 5 个内核路径中，符合预期）。至此整数域已无
任何需要整体回退解释器的指令形态，"1:1 翻译 + 少量 fallback"（fallback 仅剩
v128 与罕见形态）在整数域闭环。

### 迭代 5：return_call / return_call_indirect 真尾调用（已完成，2026-09-21）

解码与校验层此前已支持（含结果类型一致性检查），本轮补齐执行层：

- **解释器**：return_call 到解释器 callee 走**真尾跳**——参数下沉到当前帧基址后
  原地重载函数上下文（inst/code/base/locals/ctl[0]/pc），帧复用、深度不增长；
  到 JIT callee 走 call+尾返回（C 栈每层约 700B，测试深度内安全）。
- **JIT**：新 `emit_tail_call`——参数从操作数栈拷贝至 `[entry-a*16, entry)`
  （fp 相对寻址；操作数栈锚在帧下方 opbase，与 entry 区天然不重叠，升序拷贝安全），
  弹出自身帧（x29/x30 还原）后 sp 落在 `entry`，`br x1` 尾跳 JIT callee，
  或经 interp 桥执行后直接 `ret` 回**本函数的调用者**（校验保证结果类型一致，
  结果恰好落在调用者期望的槽位）。C 栈零增长，百万层尾递归瞬时完成。
- **顺带修复三个存量缺陷**（都在本轮测试中暴露）：
  1. trampoline 只保存 x29/x30，JIT 代码摧毁 x19-x28——解释器用这些寄存器持有
     S/pc/ctl，跨 `ea_jit_call` 后全部变垃圾（interp→JIT 调用即崩）。trampoline
     帧扩至 128B 完整保存 x19-x28，恢复 C ABI 契约；
  2. 解释器调用 JIT callee 前未同步 `ex->sp`，且返回后未刷新局部 `S/sp`
     （np≠nr 时读到错误结果位）——四处调用点统一同步；
  3. `emit_call_indirect` 传给 `ea_jit_callee_lookup` 的 table_idx/elem_idx
     参数对调（单表小索引侥幸可用，多表模块必错）。

验证：tail.wasm 全形态用例（直接/间接/互递归/JIT-callee/interp-callee，
1M 深度尾递归）JIT==int；官方 return_call/return_call_indirect 两模式逐字节一致
（剩余失败为 GC 类型语法解码，属二阶段）；全量套件 170/258 失败集不变；
5 内核数值一致、性能持平。

### 与 wasmtime 的两维度对比（2026-09-21，wasmtime 48.0.2 / easm 同机同套件）

**覆盖率**（官方 spec 套件 `tests/spec/test/core`，258 个 wast 文件）：

| 口径 | easm | wasmtime（-W 全特性开启） |
|---|---|---|
| 严格（文件内全部指令零失败） | **170 / 258（65.9%）** | **218 / 258（84.5%）** |
| 语义口径* | 170 / 258（wast2json 路径天然跳过文案断言） | **257 / 258（99.6%）** |
| 断言级（233 个可转换文件的 assert 命令） | 57,483 | 60,168 |

\* 语义口径：`assert_invalid/assert_malformed` 仅因报错文案与 spec 解释器措辞不同而失败的
视为通过（easm 的 wast2json 驱动本就不做文案匹配）。wasmtime 40 个严格失败里 39 个属此类；
唯一真实失败是 `gc/type-subtyping.wast`——wasmtime 自家 wat 解析器不支持 `(sub $p1 $p2 …)`
多父继承语法，属其对等缺陷。

差距结构（easm 88 个未过文件）：GC/sub/rec 类型语法（25 个文件连 wast2json 都无法转换，
wasmtime 原生解析其中 4 个）、call_ref / function-references 组、relaxed-simd（6）、
exceptions（2）、annotations（wasmtime 亦失败）、其余为 memory64/解析严格性边角。
性能维度对比与覆盖率结论支持同一判断：MVP+SIMD+尾调用主干已闭环，下一阶段的主投入点
是 GC 类型语法与 function-references（覆盖率 +6~10 文件的钥匙），以及 HIR/寄存器分配
（性能差距的主因）。

**性能**（秒，best-of-2，含引擎启动；同机 M 系列 aarch64）：

| kernel | easm-jit | easm-int | wasmtime | easm-jit vs wasmtime |
|---|---|---|---|---|
| fib（10M 次迭代） | 0.023 | 0.446 | 0.008 | 慢 2.9× |
| primes（500K 筛） | 0.009 | 0.308 | 0.005 | 慢 1.8× |
| sum（100M i64） | 0.181 | 8.985 | 0.020 | 慢 9.1× |
| matmul（30×128³） | 0.351 | 17.390 | 0.029 | 慢 12.1× |
| memsum（2M load） | **0.003** | 0.007 | 0.005 | **快 1.7×** |
| 启动 + trivial 调用（中位，ms） | **2.4** | **2.2** | 4.4 | **快 1.8×** |

解读：
1. easm JIT 保持对解释器 17–49× 的加速；分支密集内核（fib/primes）与 wasmtime 差距已收敛到
   1.8–2.9×，且全部快于 node；memsum 反超 wasmtime（内存带宽瓶颈 + 更轻的 CLI 启动）。
2. 计算密集内核（sum/matmul）仍慢 9–12×：Cranelift 有完整寄存器分配、循环展开与
   NEON 向量化；这是 HIR/寄存器分配阶段的预期收益空间。
3. 启动开销 easm 更低（无 Cranelift 初始化，符合"嵌入式、快预热 <50ms"的定位）。

**内存开销**（峰值 RSS，macOS `/usr/bin/time -l` ru_maxrss，3 次取中位，单位 MB）：

| 场景 | easm-jit | easm-int | wasmtime | vs wasmtime |
|---|---|---|---|---|
| 启动 + trivial 调用（fib 2） | **1.7** | **1.6** | 7.4 | 基线为其 **1/4.4** |
| fib（10M，计算密集） | 1.7 | 1.6 | 7.4 | — |
| primes（500K 筛，含访存） | 2.2 | 2.1 | 7.9 | — |
| sum（100M i64） | 1.7 | 1.6 | 7.4 | — |
| matmul（30×128³，访存密集） | 1.9 | 1.8 | 7.6 | — |
| memsum（2M load） | 1.8 | 1.6 | 7.5 | — |
| **负载峰值** | **2.2** | **2.1** | **7.9** | 峰值为其 **1/3.6** |

- 磁盘足迹：easm 可执行文件 189KB，wasmtime 单二进制 48.5MB（差 256×）。
- 各内核的 wasm 线性内存 ≤2MB，故 RSS 差异全部来自引擎自身开销：wasmtime 的基线
  主要是 Cranelift 编译器基础设施；easm 的增量是 16MB MAP_JIT 代码区与 12GiB
  地址空间预留的**已触碰页**（虚拟预留不计入 RSS）。
- 结论：内存开销上 easm 与"嵌入式/embedded"定位一致——基线 1.7MB、最重内核 2.2MB，
  较 wasmtime 低 3.6–4.4×，较 V8/node（基线 39MB）低约 20×。

## 六、下一步（按规划优先级）

1. **HIR / 寄存器分配**（性能阶段主战场）：当前 1:1 栈机翻译在 sum/matmul 上与
   Cranelift 的差距主要来自冗余 push/pop；引入虚拟寄存器 + 线性扫描可预期再获 2–5×。
2. 浮点参数直接 S/D 寄存器往返（省 fmov gpr↔fpr 两次搬运）。
3. br_table / v128 的 JIT lowering（当前回退解释器）。
4. 二阶段特性：function-references、EH、tail-call 的解码/校验/执行支持，
   预计可将覆盖率推向 230+/258。
5. 校验器严格性补齐（align 等 assert_invalid/malformed 细项）。

## 七、复现命令

```bash
make -j8                                     # 构建（188KB）
python3 tools/run_spec.py -j 8 --jit         # JIT 模式一致性覆盖率 → build/coverage_report.md
python3 tools/run_bench.py all               # JIT/解释器/wasmtime/node 对比
EA_JIT=1 build/easm run bench/bench.wasm primes 500000   # 单内核运行
EA_JIT_DUMP=1 EA_JIT=1 build/easm run x.wasm f   # dump JIT 机器码（llvm-mc 反汇编可读）
```
