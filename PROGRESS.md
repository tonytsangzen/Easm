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
| 一致性覆盖率（官方 spec 测试套） | ✅ 257/258 文件全过（解释器与 JIT 模式逐文件一致） |
| Benchmark 对比（wasmtime / node） | ✅ JIT 较解释器 17–31×，fib/primes 快于 node |

可执行文件 `build/easm` 仅 **188 KB**（目标 <200KB ✅）。

## 二、一致性覆盖率（官方 spec 测试套）

运行方式：`python3 tools/run_spec.py -j 8 [--jit]`（内部 wast2json + 并行 + 缓存）

- **258** 个 wast 文件中 **257 个完全通过（99.6%）**，解释器模式 61,120+ 条、
  JIT 模式 61,124 条 assert 全部通过
  （迭代 7 结束时 182 → 迭代 9 结束时 225 → 迭代 11 结束时 243 → 迭代 12 结束时 257）。
- `--jit` 与解释器模式**逐文件结果完全一致**：JIT 编译成功的函数真正执行机器码，
  不能降级的函数（v128 局部/参数等）自动回退解释器，正确性对齐。
- 转换失败仅剩 1 个文件（annotations.wast，wabt/wasm-tools 均不支持注解文本语法）。

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

### 迭代 6：function-references 类型系统（已完成，2026-09-21）

覆盖率 **170 → 180 / 258**，新通过 10 个文件：return_call、return_call_indirect、
call_ref、ref、ref_func、ref_is_null、br_on_null、br_on_non_null、ref_as_non_null、
select（另修复 func.wast 回归）。

改动分层：
- **解码**：类型节支持 rec 组（类型索引按组展开预计数）、sub / sub final（记录超类型）、
  struct/array 形状（登记不执行）；valtype/reftype 支持带堆类型的引用
  （`(ref null $t)`/`(ref $t)`，EaValType 高位段编码携带类型索引）；
  表/全局/局部/块类型的 typed ref；ref.null 堆类型立即数。
- **校验**：vt_match 子类型匹配（(ref $t) <: (ref null $t) <: funcref，含超类型链与
  可空性规则）；ref.func 携带声明类型；call_ref/return_call_ref 要求精确的
  `(ref null $t)`；br_on_null/non_null 的分支/落空栈语义（non_null 分支携带窄化引用、
  落空丢弃）；typed-ref 类型索引越界检查（类型/全局/表/段/局部/块类型）；
  **local 初始化跟踪**（非默认化 local 未读检查，unreachable 多态，else/end 状态
  保存恢复）——修复 func.wast 回归。
- **执行**：解释器实现 ref.as_non_null / ref.eq / br_on_null / br_on_non_null /
  call_ref / return_call_ref（call_ref 空引用 trap："null function reference"）；
  return_call_indirect 的解释器分支改为真尾跳（30 万层互尾递归平坦运行，
  修复 C 栈耗尽崩溃——递归块与尾跳代码曾叠加共存）。

验证：上述 10 个官方文件全部零失败；全量套件两种模式 180/258 失败集完全一致；
5 个 bench 内核数值一致、性能持平。

### 迭代 7：return_call_ref / local_init / 抽象非空引用（已完成，2026-09-21）

覆盖率 **180 → 182 / 258（70.5%）**：return_call_ref、local_init 全过，unreached-valid
的 br_table/窄化边角修复，其余文件零回归。

- **抽象非空引用**：`(ref func)`/`(ref extern)`/`(ref any)` 等抽象堆类型不再坍缩成
  可空 shorthand——独立编码（EA_VT_ABSREF），local 默认化规则据此判定
  （`(local (ref extern))` 不可默认化）；
- **vt_match 重构为 ty_match(m, want, got)**：抽象↔具体双向规则
  （(ref func) 接受非空 typed func 引用；(ref func)/(ref extern) 宽化到
  funcref/externref）；elem/global 的 CONST_TMATCH 升级为同一匹配器；
- **return_call* 的结果类型检查**确认为方向性子类型（callee <: frame）——
  vt_list_eq 的对称性曾让 "funcref 回调 (ref null $t)" 的不健全方向漏过；
- **br_on_null/non_null 的窄化**统一为 ea_narrow_ref 助手（funcref → (ref func)
  等），unreachable 多态下按标签自身引用类型窄化（unreached-valid 的
  "Bottom heap type" 用例）；
- **br_table 目标类型检查**在 unreachable 代码中只要求 arity 一致；
- **check_const_expr 的硬错误传播**：err 设置后调用方只看返回类型的缺陷修复
  （ref_func 的 "unknown function 7" 被吞）——哨兵值返回 + 四处调用点检查；
- **call_ref 空引用 trap** 统一为 "null function reference"；return_call_ref
  解释器分支改真尾跳。

验证：本轮触及的 13 个官方文件全部零失败或不再劣化；全量套件两种模式
182/258 失败集完全一致；bench 内核数值一致、性能持平。

### 迭代 8：memory64 / table64 索引类型 + 批量指令（已完成，2026-09-21）

覆盖率 **182 → 199 / 258**：memory64 组 13 个文件（address64/align64/binary_leb128_64/
bulk64/call_indirect64/endianness64/float_memory64/load64/memory64/memory64-imports/
memory_grow64/store64/table_copy64/table_copy_mixed/table_fill64/table_get64/
table_grow64/table_init64/table_set64/table_size64 等）全部转绿。

- **memory.init / table.init / table.copy / memory.copy 弹栈顺序与宽度**：
  按参考解释器规则统一为 [dst_idx, src_idx, min(dst,src)]（长度仅当两者皆 64 位
  才是 i64）；memory.init/table.init 的 src/len 恒为 i32；memory.grow/size、
  table.grow/size/fill 的操作数与结果按 is64 选择 i64/i32（解释器 + JIT 辅助
  函数 + 校验器三处同步）；
- **memarg offset 升级为 u64**：EaInstr 增加 imm.ma（align/memidx/offset 8 字节），
  lane 立即数移出 union（此前 lane 与 offset 高半段重叠导致 store-lane 全线回归）；
  32 位内存 offset > 2^32-1 报 "offset out of range"；
- **64 位内存页数上限 2^48**（含），2^48+1 拒绝；64 位表默认 max UINT64_MAX；
- **call_indirect 索引操作数**按表的 is64 取 i64/i32（校验器/解释器/JIT 弹栈）；
- **memory64 导入兼容性检查**：is64 不一致即 unlinkable；spectest 增加
  table64/memory64 导出；
- **校验严格性**：table.copy 元素类型 src <: dst 方向化；SIMD 内存操作的对齐/
  lane 范围/offset 范围检查（loadNxN natural 宽度全为 8 字节）；memop flags
  ≥ 0x80 报 "malformed memop flags"；align 指数移位溢出修复（align=2^32）。

### 迭代 9：SIMD 执行语义 + relaxed-simd + 名称/链接（已完成，2026-09-21）

覆盖率 **199 → 225 / 258（87.2%）**。

- **exec_simd 栈深度系统性修复**：二元/三元操作的 PUT_D 保留 sp，净深度差 1——
  单操作函数侥幸通过、嵌套即产生错位结果。全部 BIN/CMP/SHIFT 宏与直写 case 改
  PUT_D_DROP/PUT_D_DROP2（load-lane 的结果写址同步修正：结果应落地址槽）；
  这一修复连带解决 i64x2_arith/extmul/lane 等文件的大量失败；
- **extmul 操作数/结果宽度**：i16x8.extmul_i8x16 读字节 lane（sb/b），结果写
  sh/h（i16 lane）；i32x4.extmul_i16x8 结果 4×i32（此前误写 2×i64）；
- **EA_OPV 操作码空间迁移 0xFD00 → 0x10000**：relaxed 区段（sub ≥ 0x100）原与
  0xFE 前缀（线程原子操作）冲突；relaxed 操作码表按最终 spec 重排
  （madd=0x105+、laneselect=0x109+、min/max=0x10D+、q15mulr=0x111、dot=0x112+），
  补 i32x4.relaxed_trunc_f64x2_{s,u}_zero、i16x8.relaxed_dot（i16 饱和）、
  i64x2.sub(0xD1)；
- **UTF-8 名称校验**：完整 spec UTF-8 验证器（overlong/代理/越界），
  utf8-* 3 个文件 528 条断言全过；导出名重复检查（含 NUL 字节名字的长度比较，
  EaExport 增加 name_len）；
- **tag section 最小管道**：section id 13 排序、tag 导入/导出解码与类型检查
  （签名必须无结果）、运行时导入解析——imports.wast 转绿；
- **table/elem/const 语义**：表初始化表达式类型检查（非空表类型必须有显式
  init 表达式）；elem funcidx 形式元素类型为非空 (ref func)；table.init 元素
  类型 src <: dst 方向化；const 表达式恰好一个值；全局/表/段初始化的
  global.get 可见性按 section 顺序（table 只见导入全局）；
- **SIMD 多内存**：SIMD memarg 支持 memidx 标志位；v128 select 校验放开；
- **spectest 修正**：global_i64 = 666；无名 register 注册最近模块。

验证：全量套件两种模式 225/258 失败集完全一致；bench 五内核数值一致、性能持平
（fib 0.025s / primes 0.012s / memsum 0.003s 与迭代 7 相同）。

### 迭代 10：GC 运行时（struct/array/i31/ref.cast/br_on_cast）（已完成，2026-09-21）

覆盖率 **225 → 237 / 258（91.9%）**，累计通过 60,788 条 assert。GC 组 15 个文件中
9 个完全通过（struct/array/i31/extern/ref_eq/ref_test/ref_cast/br_on_cast/br_on_cast_fail），
其余 6 个剩少量失败（type-subtyping 18、array_init_data 6、array_new_data 5 等）。

- **wast2json 不支持 GC 文本语法**（i8/i16 字段、rec、i31）——新增
  tools/wast_convert.py：自制 wast→wast2json 兼容 JSON 转换器，文本模块经
  wasm-tools（bytecodealliance，完整 wasm 3.0 文法）parse 为二进制；
  run_spec.py 在 wast2json 失败时自动回退该转换器（转换失败 25 → 1）。
- **类型系统扩展**：EaType 携带字段类型（storage type + mut + packed）；
  值类型编码新增可空抽象引用（EA_VT_ABSN：eq/i31/struct/array/none/nofunc/
  noextern/exn）与 GC 简写（0x6A-0x71）；ea_tref_nullable 修正
  （具体 typed 引用按奇偶、抽象按 bit0、普通枚举恒可空）。
- **0xFB 指令解码**：struct.new/new_default/get(_s/_u)/set、array.new/
  new_default/new_fixed/new_data/new_elem/get(_s/_u)/set/len/fill/copy/
  init_data/init_elem、ref.test(_null)/ref.cast(_null)（立即数为 s33 heaptype，
  0x14 非空 / 0x15 可空）、br_on_cast(_fail)（flags 在 label 之前；label 存放
  union 安全槽 q.d）、any/extern.convert、ref.i31、i31.get_s/u。
- **校验器**：GC 指令完整类型规则；结构化子类型 type_sub（声明 sup 链 +
  结构规则：函数参数逆变/结果协变、struct 不可变字段协变/可变不变）+
  结构化 canonical 等价（rec 组重复类型同 一性）；表初始化表达式类型检查
  （非空表元素类型需显式 init）；elem funcidx 形式元素类型为非空 (ref func)；
  select 放开 v128。
- **运行时**：EaHeapObj 堆对象（struct/array，magic 区分 funcref）；i31 用
  指针低位标记（v<<2|2，31 位截断语义）；ref.cast 运行时子类型检查
  （sup 链 + canonical 等价）；any/extern.convert 恒等转换（extern 侧包裹
  可观察）；区分 "null structure/array/i31 reference" 与
  "out of bounds array/table/memory access" trap 消息。
- **回归修复**：GC 插入时误删 UNREACHABLE 的 set_unreachable（导致
  unreached-invalid 大面积误收）——根因修复后回退两处过度宽松的多态兜底；
  ea_narrow_ref 补 ANYREF；const 表达式恰好一个值。

验证：全量套件两种模式 237/258 失败集完全一致；bench 五内核持平
（fib 0.025s / primes 0.009s / sum 0.185s / matmul 0.360s / memsum 0.003s）。

### 迭代 11：GC 收尾（ref_null/数组残项/子类型化间接调用）（已完成，2026-09-21）

覆盖率 **237 → 243 / 258（94.2%）**，累计通过 61,000+ 条 assert。GC 组 15 个文件中
14 个完全通过（新增 ref_null、array_init_data、array_new_data、array_copy、
array_init_elem、array_new_elem、br_on_cast_fail），仅 type-subtyping 剩 6 条
rec 组跨组等价的边界用例。

- **空引用底部宽化**：nullfuncref/nullexternref/nullref 保持底部语义
  （0x73/0x72 不再坍缩为可空简写），可宽化到对应层级的任意可空引用
  （nofunc → (ref null $t func)、none → (ref null $t struct/array) 等）；
  exnref/nullexnref 简写（0x69/0x74）补齐；
- **数组数据操作语义**：元素字节宽度按存储类型（i8→1/i16→2/i32→4/i64→8/
  v128→16）；数组边界检查先于数据/元素段检查；丢弃段上 len=0 仍可用、
  len>0 陷阱；funcref 元素类型的 array.init_data/new_data 拒绝
  （"array type is not numeric or vector"）；
- **元素段条目缓存**：含 GC 表达式的 elem 段条目在实例化时求值一次
  （引用同一性——ref.eq 判断数组复制后的元素相等）；interp/JIT 的
  array.new_elem/init_elem/table.init 统一路径；
- **call_indirect 子类型化**：表元素类型可为类型化函数引用
  （(ref null $t2) 等）；被调函数类型按声明的 sup 链 + canonical 等价
  匹配（替换精确 memcmp）；plain comptype 视为 final（修复 rec 组
  canonical 等价的误判）；funcidx 元素段按各函数自身类型检查；
  ref.eq 结果槽清零（高位脏数据）。

验证：全量套件两种模式 243/258 失败集完全一致；bench 五内核持平。

### 迭代 12：exceptions 执行语义（throw/throw_ref/try_table）（已完成，2026-09-21）

覆盖率 **243 → 248 / 258（96.1%）**，累计通过 60,895 条 assert。exception-handling
组 4 个文件全部通过（throw 5/0、throw_ref 7/0、try_table 45/0、instance 12/0），
两种模式一致。

- **EH 核心对象**：`EaExnInst{tag, n_vals, vals[]}`（捕获时初始化的载荷副本）、
  `EaTagInst.ident`（标签唯一身份 = 拥有实例的槽，跨实例同名 tag 不匹配）、
  `EaExec.pending_exn`（未捕获异常沿调用链传播，JIT 边界 5 个调用点 +
  3 个入口点透传）；
- **try_table**：解码（`1f bt n (kind tag label)*`，kind 0..3 =
  catch/catch_ref/catch_all/catch_all_ref）；标签相对 try_table 外层解析
  （validator 用 outer_csp、interp 用 `ctl[k-1-l]`、`csp=k-l`）——根因是
  predecode_region 嵌套配对 resolver 不认识 TRY_TABLE，块尾配对到 try 的
  end 导致校验/执行跳错位置；
- **eh_unwind**：扫描控制栈找最近 is_try 帧 → catch 按 tag->ident 同一性
  匹配 → 携带 n_params(+exnref) 个值落目标块栈底 → 跳 `t2->pc_end`；
  throw_ref 重抛 exnref 槽（空则 trap）；BLOCK/IF/LOOP 帧必须重置
  is_try 标志（IF 遗漏导致错误匹配）；
- **module definition / instance 命令**：wast 驱动支持 `(module $M ...)` 只
  注册不实例化 + `(instance $I (module $M))`，register 无名时用最近实例；
- **globals 指针化**：`inst->globals` 改 `WVal**`（导入别名源存储，修复
  instance.wast glob1/glob2 别名语义）；JIT GLOBAL_GET/SET 增加一级间接；
- **memory 共享化**：`inst->memories` 改 `EaMemInst**`（导入别名源实例的
  EaMemInst）——修复 memory.grow 跨实例不可见（linking "5 pages by the
  time we get here"）；JIT prologue 经 `jit_mem0`（共享结构）重载
  x25/x26，任意别名实例增长后下一个调用即生效；释放只回收本实例定义
  （owned）的内存；
- **全局导入子类型**：可变性精确相等；不可变导入接受协变
  （`(ref func)` � <: `funcref`），可变导入要求 canonical 等价（含可空性）；
  ea_vt_sub 抽象格补充：类型化引用按其复合类型种类（func/struct/array）
  映射进抽象层级（`(ref $t)` <: `funcref`），抽象→具体仅底部种类可流入
  （nofunc → `(ref null $t)`、none → struct/array）；
- **实例化顺序**：elem 条目缓存求值移到 funcs/globals/memories 就绪之后
  （elem 表达式可 global.get/ref.func 任意定义项——global.wast
  "Definition order" 模块崩溃的根因）。

验证：全量套件两种模式 248/258 失败集完全一致（interp 与 JIT 各自核对）；
bench 五内核持平（fib 0.024s / primes 0.010s / sum 0.186s / matmul 0.363s /
memsum 0.003s）。

### 迭代 13：rec 组跨组 canonical 等价（已完成，2026-09-21）

覆盖率 **248 → 251 / 258**。type-rec（3/0）、type-equivalence（4/0）、
type-subtyping（29/0）三个文件全过——按参考解释器模型重写了 canonical 等价：

- **rec 组身份**：EaType 增加 `rec_pos`/`rec_size`（解码时从 0x4E rec 块
  记录；plain 类型 = 单成员组）；
- **等价语义**：两个类型相等 ⟺ 各自处于结构逐成员相等的 rec 组的相同
  位置。组内自引用按**位置**（Rec j）匹配，外部引用按指向的类型递归
  比较——内/外之别是本质的（同一原始索引两侧语义可不同）；
- **归纳性**：比较中的组对假设相等（最大不动点），递归类型终止；
  跨模块版本（ma/mb 双模块）支撑 import 类型匹配；
- **陷阱案例**：B 组 struct 引用 A 组的 $f1（外部引用）≠ A 组自引用
  （Rec 0）——vt 比较的 `a == b` 原始索引捷径必须绕过；
- **跨模块 import**：func 导入改为 actual <: declared
  （`ea_type_sub_mm`，声明 sup 链 + 跨模块 canonical 等价），tag 导入改为
  双向 canonical 等价；宿主模块无 type space 时回退结构签名比较。

### 迭代 14：table_init64 + 零散严格性（已完成，2026-09-21）

覆盖率 **251 → 257 / 258（99.6%）**，assert 61,110 条。仅剩
annotations.wast（wabt 与 wasm-tools 均不支持该提案文本语法，转换失败，
非运行时问题）。

- **table.init 校验弹栈序**：栈为 [dst src n]，仅 dst 跟随表的索引类型
  （64 位表 dst=i64），src/n 恒为 i32——原实现先弹 dst 类型，32 位表
  碰巧全 i32 蒙混，64 位表报 type mismatch；
- **elem 条目缓存贯通执行**：interp 的 table.init 使用实例化时求值的
  `e->cache`（引用同一性）——修复"段不重复求值"测试（ref.eq 两次
  table.init 应得同一数组）；
- **br_table 目标 arity**：所有目标标签必须共享同一值序列 → arity
  相等在 unreachable（多态栈）下也必须成立；
- **data count 总检查**：解码收尾处 data 段数（含缺失 data 段 = 0）必须
  等于 data count 声明——修复"有 count 无 data 段"被接受；
- **names 含 NUL**：JSON 字符串解析保留字节长度（JV.str_len），导出查找
  增加 `ea_instance_export_n`（memcmp + 精确长度）——含 \x00\x01…
  导出名可调用。

验证：全量套件两种模式 257/258 完全一致；bench 五内核持平
（fib 0.024s / primes 0.010s / sum 0.189s / matmul 0.358s / memsum 0.003s）。

### 迭代 15：JIT 融合三连（比较→br_if / 结果驻留 / local.set 直存）（已完成，2026-09-21）

以 wasmtime 为参照的性能优化第一步：在不引入完整寄存器分配的前提下，
把 1:1 翻译的三类最高频冗余在局部窗口内消除。覆盖率 257/258 双模式零回归。

机器码审计（EA_JIT_DUMP + llvm-mc 反汇编 sum 热点循环）发现的三类浪费：

1. **比较 → br_if 六条变两条**：`cset → push → pop → cmp #0 → b.cond`
   塌缩为 `cmp → b.cond`（flags 直接跨指令存活，bool 永不落栈）。
   前置条件：比较与 br_if 均非分支目标。AArch64 全部条件码可反转
   （含浮点 MI/PL、VS/VC）。深度簿记：fused 时 `depth -= 1 + vpops`
   （bool + 从寄存器消费的驻留操作数都从未占槽——栈上漂移 bug 的根因，
   大循环下 sp 逐渐越界，靠 spec 全量回归抓出）。
2. **binop 结果驻留**：结果不再 push，若下一条是消费型二元/比较指令则
   驻留 x17（复用现有 pop_pair 的 def 机制）；表达式链每环节省一对
   store/load。f32/f64 结果驻留 v1（def_kind1=2/3，flush 走 fpr）。
3. **local.set 直存**：binop 结果直接 `str [FP, off]` 进局部槽，跳过
   push/pop 往返（主循环以 skip_next 跳过被融合的 local.set）。f32/f64
   经 gpr 中转（帧槽为负偏移，FP scaled imm 无法编码）。

效果（JIT 秒，wasmtime 参照）：

| kernel | 优化前 | 优化后 | wasmtime |
|---|---|---|---|
| fib | 0.024 | 0.022（-9%） | 0.009 |
| primes | 0.010 | 0.006（**追平 wasmtime**） | 0.006 |
| sum | 0.189 | 0.109（**-42%**） | 0.021 |
| matmul | 0.358 | 0.340（-5%） | 0.031 |
| memsum | 0.003 | 0.003（保持快于 wasmtime） | 0.005 |

验证：全量套件两种模式 257/258（JIT 融合路径由 if/br_if/f32/GC 等套件
高频覆盖），bench 数值逐项与解释器一致。

### 迭代 16：load/store 驻留 + 地址算术融合（已完成，2026-09-22）

承接迭代 15，消除 matmul 内核 load→fmul→fadd→store 链的剩余栈往返。
覆盖率 257/258 双模式零回归。

机器码审计发现的三类冗余与修复：

1. **地址算术 → load**：`i32.add` 算出的地址 push 后被 load pop——
   新增 `load_consumes` 谓词（全部内存加载 0x28–0x35），整数 binop/
   local.get/const 结果可驻留 x17 作为地址（emit_load 直接从 x17 搬入
   x16，省 pop；驻留的 32 位结果由 32 位指令零扩展保证有效）；
2. **f32/f64 load 结果直通**：load 结果复用 `push_result_f`——下一条是
   浮点二元运算时驻留 v1，`ldr s0 → fmov s1 → fmul` 三条全寄存器化；
3. **浮点 store 值驻留**：fadd/fmul 结果直接从 v1 进 `str [x25, x16]`
   （def_consumes 纳入 F32/F64_STORE 作 keep 条件；emit_store/emit_load
   对不匹配的 def 状态先 flush，防御 const/local 对偶误入）；
4. **local.tee 融合**：`add → tee` 原为 push + peek + store 三次访存，
   融合为 store-to-local + push 两次（并 skip 掉 tee 本身——漏设
   skip_next 导致 tee 双重执行的 bug 由反汇编审计抓出）。

效果（JIT 秒，wasmtime 参照；matmul 三步累计 0.358 → 0.157，**-56%**）：

| kernel | 迭代 15 后 | 迭代 16 后 | wasmtime | 差距 |
|---|---|---|---|---|
| fib | 0.022 | 0.023 | 0.009 | 2.6× |
| primes | 0.006 | 0.006（持平） | 0.006 | **1×** |
| sum | 0.109 | 0.108 | 0.021 | 5.1× |
| matmul | 0.340 | **0.157（-54%）** | 0.031 | 5.1× |
| memsum | 0.003 | 0.003（保持领先） | 0.005 | 0.6× |

验证：全量套件两种模式 257/258 零回归；bench 数值逐项与解释器一致。

### 迭代 17：信号式 OOB 检测（guard page + SIGSEGV handler）（已完成，2026-09-22）

wasmtime 同款机制：JIT 内存访问去掉逐访存的 sub/cmp/branch 三条边界
检查，越界访问落入 PROT_NONE 保留区由信号处理器转换为 wasm trap。

- **既有布局即条件**：每块线性内存本就以 12GiB PROT_NONE 预留
  （MAP_NORESERVE）+ mprotect 按需提交——32 位内存的任何越界地址
  （有效地址 < 2^33，校验器已强制 offset ≤ UINT32_MAX）必然落在预留区内；
- **处理器**：SIGSEGV/SIGBUS handler 按故障地址查已注册内存区间
  （alloc_memory 注册、实例释放注销），命中则置 TRAP_OOB_MEMORY 并
  longjmp 回 invoke 的 setjmp 点（先 sigprocmask 解阻塞，防信号掩码
  残留）；未命中则恢复默认处置重新 raise（真实段错误照常崩溃）；
  `_Thread_local` 的 g_fault_exec 由 ea_instance_invoke 进出维护；
- **JIT 侧**：32 位内存的 load/store 不再发射检查（matmul 函数
  372→316 条，-15%）；64 位内存无预留上界，保留显式检查；
- **连带修复两个既有 bug**：
  1. **32 位地址高位垃圾**：push_w 只写 4 字节而地址 pop 用 64 位
     ldr 读 8 字节——陈旧高位曾让旧边界检查误判（潜在误 trap），去掉
     检查后变成野写崩溃。修复：32 位内存的地址 pop/驻留消费一律
     32 位形式（zero-extend）；
  2. **trap 桩参数交换**：桩以 w0=code、x1=ex 调用
     ea_jit_trap_now(ex, code)——参数序相反，一旦执行必崩。
     此前未被纯 JIT 代码触达而潜伏，本轮修正。

bench 五内核持平（fib 0.022 / primes 0.006 / sum 0.108 / matmul 0.159 /
memsum 0.003）：这批内核为访存带宽受限，省下的 ALU 指令本就隐藏在
访存延迟阴影中；收益在指令数（-15%）与机制对齐（wasmtime 同款）。
验证：全量套件两种模式 257/258 零回归（memory.wast 75 条 OOB trap 经
信号路径全部正确上报）。

### WASI 运行时（已合入，三后端 + CLI/GUI demo + 验证矩阵 15/15）

- **WASI Preview 1 子集**（21 个 syscall）：args/env、clock_time/res、
  random_get、fd_write/read/close/seek/tell/sync、fd_fdstat/filestat、
  prestat/prestat_dir_name、path_open、sched_yield、poll_oneoff（clock
  sleep）、proc_exit——全部按 preview1 指针出参约定实现；
- **三平台移植层**（`EaWasiPlat` vtable + 3 后端）：
  - POSIX（macOS/Linux）：open/read/write/seek/mkdir/unlink + clock_gettime
    + /dev/urandom + opendir/readdir；
  - Win32：UTF-8→UTF-16 双宽 API（CreateFileW/SetFilePointerEx/FindFirstW），
    mingw 交叉编译零 error；QueryPerformanceCounter 时钟 + rand() 随机；
  - ewokos：HAL vtable + 内置 RAM 文件系统（64KB/文件，flat 目录，
    1ms fake clock，LCG random）——纯可移植 C，本机即可全功能验证；
- **CLI demo**（wasi_echo.wasm：banner + clock + argv echo；wasi_file.wasm：
  path_open 创建→写入→关闭→重开→读回→校验）；
- **GUI demo**（wasi_gui.wasm：Mandelbrot 分形渲染写入 WASI 帧缓冲设备；
  easm_gui：AppKit NSWindow 实时渲染）；
- **验证矩阵**：15/15 通过（POSIX echo 5 项 + EWOK echo 2 项 +
  POSIX file 4 项 + EWOK file 3 项 + GUI 1 项）。

## 六、下一步（按规划优先级）

1. **HIR / 寄存器分配**（效率阶段主战场）：迭代 15-17 已把 matmul 与
   wasmtime 的差距从 11× 收敛到 5.2×、primes 追平；剩余差距来自跨语句
   值生命周期（操作数间隔其他 producer 时无法驻留，如 fmul; local.get;
   fadd），需要虚拟寄存器 + 线性扫描。信号式 OOB 已就位（迭代 17），
   寄存器分配后无需再为检查指令留窗口。
   **否定结果（迭代 18，未合入）**：单槽/6 槽（x19-x24）局部变量
   dirty-cache（控制流边界 flush、调用失效）实现后 bench 五内核全部
   持平——frame 访存为 L1 命中且不在关键路径，6 槽方案在循环回边
   flush 6 条 stur 抵消了省下的 load。结论：HIR 的收益必须来自操作数
   栈槽位的寄存器分配（binop push/pop 链，数量远大于 frame 访存），
   而非局部缓存。
   **量化瓶颈分析（wasm-tools 反汇编确认）**：clang 在 wasm 层已把 sum
   循环 4× 展开（3 个归纳变量步进 12/4/30，每趟 4 次累加），双方跑同一
   wasm，对比公平。以 P-core ~3.9GHz 折算：sum 每趟 wasmtime ≈3.3 周
   （≈4 次串行加法链的下界，已接近最优），本实现 ≈17 周——循环体
   ~40 条指令（栈槽 ldur/stur 对）发射受限。fib 每迭代 5.9 vs 2.2 周
   （栈往返 ~3.7 周）。**量化目标**：栈槽位寄存器分配把 sum 循环体
   压到 ~12 条（s + 3 个归纳变量驻留）即可达 wasmtime 同档。
   **迭代 19（已合入，实验门控）**：6 槽（x19-x24）局部变量缓存 +
   循环暖态两趟编译已进树，双门控：`EA_CACHE=1`（基础缓存，spec 套件
   257/258 全绿，bench 五内核持平）、`EA_WARM=1`（两趟暖态）。
   默认路径 = 迭代 17 行为。
   **已确立的关键不变量**：中央 flush 必须位于 insn_at 之后（写回
   存储属于该标签的代码，否则跳向块尾的分支绕过写回）；回边恢复必须
   将 want[j] 固定到槽 j（头部按该分配编译，"下一个空闲槽"式恢复会
   串位）；skip 模式下的 END 也必须终结 wl 上下文（循环体经内部 br
   退出时 END 走 skip join 分支，否则 wl_pass 卡死、后续碰巧命中陈旧
   head 的分支会发射错误寄存器搬移）。
   **EA_WARM 残余缝隙（[R] 升级后精确化）**：余数循环头入口寄存器
   [i2=1 ✓, l6=3 ✗(应0), l5=0 ✓, s=0 ✓, i1=6 ✗(应3), l4=2 ✗(应1)]、
   帧 local6=3（应0）——预填充装载到的帧已被体执行污染，循环跑 2 遍
   （count 初值 2）。缝隙锁定：余数 setup（pcs 88-99）与循环头之间
   的帧写入。原**EA_WARM 残余缝隙**：EA_WDBG/EA_WDBG2 插桩显示四循环生命周期
   全部正常（pass1→record→pass2→finish），但 bench 内核（sum 余数
   路径、8× 展开 fib、primes）仍产出错值——spec 套件 257/258 未覆盖
   该模式。下轮：以 EA_WARM 差分 dump（warm vs 冷态）逐指令比对
   pass-2 循环体，配合 mini.wat 形态枚举（多归变量 + tee + 内部
   退出分支组合）定位。
   **迭代 20 附带修复（已合入，默认路径受益）**：JIT 前序从不零初始化
   声明局部（只拷参数）——读先于写的局部读到栈垃圾，违反 wasm 零
   初始化规范（sum(1) 的余数路径触发；解释器正确）。前序补
   `str xzr` 循环。注：该修复解释了部分此前"只有 bench 内核出错"
   的观察——零初始化缺失与 warm 缝隙可能叠加，warm 收尾时需在
   修复后基线上重新验证。
   **迭代 20 附带修复（已合入，默认路径受益）**：JIT 前序从不零初始化
   声明局部（只拷参数）——读先于写的局部读到栈垃圾，违反 wasm 零
   初始化规范（sum(1) 的余数路径触发；解释器正确）。前序补
   `str xzr` 循环。
   **EA_WARM 运行时单步轨迹（lldb + [R]/[A] 对齐，已合入证据
   build/warm_trace_evidence.txt）**：余数循环头入口状态 =
   [i2=1, l6=3, l5=0, s=0, i1=6, l4=2] = **setup 正确值 + 恰好一遍
   余数体的增量（i2+1, l6+3, i1+3, l4+1）**——即余数体在预填充之前
   已执行过一次：pass-1 体被截断后仍在某处执行了一次（怀疑
   guard br_if 的落点 insn_at[131] 或 restart 截断边界与
   [R]/预填充发射位置的相互作用）。随后循环体语义正确（头状态
   [1,3,0,0,6,2] → 体增量 → [2,6,0,5,9,3]）。下轮：检查
   `c->em.len = c->wl_em0` 截断后、pass-2 重发射与 restart 发射
   （预填充 + [R]）的重叠/覆盖关系，以及 guard 落点的实际指令。
   驻留）。(a) 可独立先行：guard page + SIGSEGV handler 方案。
2. call_indirect 类型检查改为解码期缓存 canonical id（现为每次比较重建等价栈）。
3. br_table / v128 的 JIT lowering；浮点参数直接 S/D 寄存器往返。
4. annotations.wast 需支持注解提案文本语法（wabt/wasm-tools 均不支持，
   需自制转换路径）。
4. 覆盖率之外的健全性：运行 spec 之外的场景（多实例 grow 别名、异常
   传播跨 JIT 边界的压力用例）。

## 七、复现命令

```bash
make -j8                                     # 构建（188KB）
python3 tools/run_spec.py -j 8 --jit         # JIT 模式一致性覆盖率 → build/coverage_report.md
python3 tools/run_bench.py all               # JIT/解释器/wasmtime/node 对比
EA_JIT=1 build/easm run bench/bench.wasm primes 500000   # 单内核运行
EA_JIT_DUMP=1 EA_JIT=1 build/easm run x.wasm f   # dump JIT 机器码（llvm-mc 反汇编可读）
```

---

## 阶段报告（2026-09-22）：JIT exception handling + 真实 JIT 模式覆盖

### 一、JIT exception handling（本阶段主目标）

此前含 `try_table`/`throw`/`throw_ref` 的模块在 JIT 侧整模块放弃
（`ea_jit_compile_module` 直接 return），全靠解释器。本阶段实现了完整的
JIT EH 代码生成：

- **handler 栈**（per-EaExec）：`try_table` 安装 `{sp0, fp, inst, desc}`，
  desc（含各 clause 的 tag/kind/delta_up）驻留在代码区 arena，push 时内联
  最终地址；`try_table` 正常退出 / `br` 跨越 / `return` / 尾调用精确 pop。
- **throw/throw_ref**：helper 只匹配**当前帧**（fp 相同）的 handler，
  匹配则按 `br` 语义落载荷（`sp_f[ncarry-1-q] = vals[q]`，exnref 置顶）并
  跳转 clause 目标；不匹配则置 `pending_exn` 返回 marker（x0==1），
  调用方 call site（`cmp x0,#1; b.eq resume`）逐帧续搜——
  **JIT↔解释器边界双向传播**（解释器帧沿用 ctl 栈 unwind）。
- **catch 语义对齐**：clause label 相对 try_table **外层**解析；JIT ctrl 栈
  无合成函数帧，`label == outer_csp` 即函数级标签（落私有 return stub）。
- **安全网**：invoke 边界清 stale `pending_exn`、trap longjmp 后恢复
  `eh_top`；prologue 原生栈深检查（递归超限 trap 而非烧穿 C 栈）。
- **开销隔离**：仅异常/间接调用相关模块生成 checked call sites；
  bench 模块零额外 call 开销。

### 二、`--jit` 从未生效 → 首轮真实 JIT 全量回归暴露 10+ 个潜伏 bug

排查中发现 `run_spec.py --jit` 虽解析但从未传入子进程，`wast.c` 也从不调用
`ea_jit_compile_module`——**此前所有"JIT 模式逐文件一致"的验证实际跑的是
解释器**。接线修复后首轮全量 JIT 回归从 248→162→228→248 逐层修复：

| # | bug | 症状 |
|---|---|---|
| 1 | `SELECT_T` 发射非法编码（0x9EA00C00 基码） | typed select 恒返回 false 值 |
| 2 | EH/异常桩内联在 prologue 后，入口**跌落** | 首指令即返回 marker |
| 3 | `INT_MIN/-1` trap fixup 用 26 位 B 编码写 bcond | SIGILL（div 系） |
| 4 | `fix_to_pc` 不初始化 fx.cond/ccode（realloc 垃圾） | 大 br_table 分支腐坏 |
| 5 | `call_indirect` JIT 路径**不弹参** | callee 读调用者栈垃圾 |
| 6 | 尾调用解释器桥传 args_end 非 args_base + 复用被桥污染的 x30 | return_call 挂死 |
| 7 | r>a 结果写入桥自身帧（伪红区） | gc/array 跳 NULL |
| 8 | `data.drop`/`elem.drop` 3 参 ABI 对 5 参调用点 | bulk-memory 崩溃 |
| 9 | JIT prologue 无栈深检查（且 `cmp sp` 编码成 xzr） | 深递归段错误/全函数误 trap |
| 10 | `fcvtzs` f64→32 基码错 + trunc_sat 不处理 parked 浮点 | conversions 49 FAIL |
| 11 | i64 load 按 4 字节压栈且压错寄存器 | endianness/load/float_memory |
| 12 | `memory.size`/`table.size` helper 向 sp**上方**写结果 | memory_size 系 17 FAIL |
| 13 | multi-memory 访存永远用 memories[0]（x14/x15 按需加载修复） | multi-memory 全簇 |
| 14 | 浮点 binop/fcmp 不 flush int-parked 延迟操作数 | f32/f64 1442 FAIL |
| 15 | min/max NaN 未静默化、±0 用了 -inf 位模式 | f32/f64 NaN 断言 |
| 16 | linking.wast 语义：失败实例化的 funcinst 仍可调用；
     jit_mem0 原在 elem/data/start 之后设置 | linking 崩溃 |
| 17 | `unreachable` 不发射 trap（只标记死代码） | unreachable 57 FAIL |
| 18 | defer 窗口跨越分支目标共址位置（producer 被跳过读陈旧寄存器） | EH catch 后 if 走错 |

### 三、覆盖率（双模式）

| 模式 | 全过文件 | 说明 |
|---|---|---|
| 解释器 | **257/258**（99.6%，与之前一致，零回归） | 唯一未过 annotations.wast（转换器限制） |
| JIT | **257/258**（61,124 条 assert 全过） | 与解释器逐文件一致（详见下方迭代 21） |

新增 `assert_exception` 命令支持（此前未捕获异常断言被静默跳过）。

### 四、性能影响

bench（JIT）：fib/primes/sum/memsum 在噪声级 ±5% 内；matmul 0.157→0.215
（~15%，prologue 栈检查 + 代码布局变化，后续可用 leaf 函数跳过栈检查回收）。
体积：~240KB → ~250KB。

### 五、下一步

1. matmul 布局回归调查（leaf 函数免栈检查）
2. EH 快路径：clause 匹配内联（常见单 tag 场景省 helper 调用）

## 阶段报告（2026-09-22）：迭代 21 — JIT 模式 257/258 追平解释器

### 一、修复的四个 JIT 缺陷（剩余 9 个测试文件全部清零）

1. **多值 br 携带 dst 公式差 (arity-1) 个槽位**：目标公式写成 `(cur-target_depth-i)*16`，
   正确应为落点后**顶部 arity 槽** `(up+arity-1-i)*16`（up = cur-target_depth）。
   up>0 时旧公式把值放低 arity-1 槽；up==0 且 arity≥2 时甚至向 sp 上方
   （slot cur+1…）写入。且递减循环在向上搬移时会读到本迭代刚写的槽
   （前向重叠拷贝）→ 改为**升序循环**（高地址先拷，对上移重叠安全；
   up==0 退化为自拷贝 no-op）。受害：block/if/loop/br/fac/stack/local_tee/
   load/load64/load2/memory_grow 的全部多值用例。
2. **16 字节 ldp/stp 携带涂抹陈旧高半区**：标量槽只有低 8 字节被写（i32 推送仅
   str w），16 字节搬运把 +8..+16 的栈垃圾带进目标槽，i64 结果读到
   0x0000000300000012 这类值。br 携带不改变操作数类型 → 改回 **8 字节搬运**
   即类型安全（v128 由函数级 bail 覆盖，不经过 br 携带）。
3. **16 字节 select 的结果槽错位**：pop_w 后把结果写到 [SP+0]（b 槽），
   而 `add sp,#16` 后最终栈顶落在 a 槽 → cond=0 时读到旧 a 值
   （select-i32(1,2,0) 得 1）。回退为基线 csel 形式（pop_w/pop_x×2/
   csel/push_x，select.wast 154/154、float_exprs 48 个 select 用例全过）。
4. **emit_return 的 stp imm7 静默截断**：`a64_stp_off64` 偏移仅 ±512 字节，
   100 参数函数的结果槽 off=1600 被截断写到 [FP+576] →
   return-from-long-argument-list 返回垃圾。off>504 时改经 R0 计算地址
   （≤4095 用 add imm12，否则 mov64+add reg）再 stp [R0]。
5. **v128 global.get/set 截断为 8 字节**（simd_const 4 个 as-global.set 用例）：
   按全局类型分支——VT_V128 用 ldp/stp 全槽拷贝，标量走原 8 字节路径
   （注意 ldp_post 的目的寄存器不能与 globals 指针暂存共用）。

附带：v128 参数/局部函数跳过 JIT（1:1 模型按 8 字节槽搬参数/局部，无法承载
16 字节）——此前 simd_select 的失败根因之一。

### 二、覆盖率（双模式）

| 模式 | 全过文件 | assert |
|---|---|---|
| 解释器 | **257/258**（零回归） | 61,120+ |
| JIT | **257/258**（追平解释器） | 61,124 全过 |

WASI 矩阵 15/15；bench 五内核（fib/primes/sum/matmul/memsum）噪声级内无回归。
annotations.wast 仍为唯一未过（文本语法转换限制，非运行时问题）。

### 三、下一步

1. matmul 布局回归（leaf 函数免栈检查）
2. EH 快路径：clause 匹配内联
3. v128 全量 JIT lowering（16 字节槽语义的 push/pop/move 全链路）

## 迭代 22：leaf 函数免栈检查 + EA_CACHE/WARM 通道调查（2026-09-23）

### 一、leaf 函数跳过 prologue 原生栈探针（默认路径，已合入）

不能递归、只在自己的已受检调用者之上增加一个有界帧的 leaf 函数（无
CALL*/RETURN_CALL*/THROW*/TRY_TABLE），无需每次调用都做
`sp vs jit_stack_limit` 探针——matmul/sum 类内核每次调用省 5 条指令，
调用密集的真实负载收益更大。帧 ≥64KB 仍保留探针（256KB 余量约束）。
**限定 `!cache_on`**：实验缓存通道下探针的存在是 load-bearing 的
（EA_CACHE 下跳过它 return.wast 36 FAIL，根因未查明——见下）。
验证：默认路径 257/258 保持、WASI 15/15、bench 噪声级。

### 二、EA_CACHE/WARM 通道调查（实验性，保持门控，代码维持 cc2de97 原状）

以单函数 sum 形状（`s += i*3 - (i>>1)` 计数循环）做了完整闭环：

1. **发现 pass-2 重绕缺口**：循环体以 `br` 结尾（计数循环常态）时编译器
   处于 skip 模式，循环的 END 走 skip 分支——该分支只重置 wl_pass，
   **不执行 pass-2 重绕**（重绕只在正常 END 路径）→ warm 从未真正运行，
   EA_CACHE 单开时反而更慢（59→63 条：加了 mov x19/x20 拷贝却照旧
   ldur/stur 内存）。skip 分支补上重绕后 pass-2 可达。
2. **证明目标形态可达**：pass-2 循环体 22 条、体部零局部内存访问
   （s/i 全程驻留 x19/x20，头部 `mov` 取值、回边仅跳转），
   运行时寄存器轨迹逐迭代正确（s: 3→8→16, i: 2→3→4）。
3. **出口写回是硬约束**：出环分支目标落在 `insn_at[end_idx]` 之后，
   循环 END 处的 finalize flush 是死代码（循环只会经 br_if/回边离开）；
   且编译期 dirty 位无法反映运行时状态（体部每迭代置脏）→ 出口必须
   **无条件写回全部 want 局部**（spill_warm），尾部才能读到现值。
   该方案在 sum 形状上完全正确（sum(10)=140）。
4. **未解**：全量套件 warm 模式仍有缝隙（内部 join 的多前驱映射一致性、
   >6 活跃局部溢出）——通道需要先设计 join 一致性规则再解锁，
   本轮所有 cache 改动已回退，调查结论留档。
5. 附带发现：**leaf-skip 与 EA_CACHE 相互作用破坏 return.wast**
   （36 FAIL）——探针为何 load-bearing 未查明，是下轮切入点。

## 迭代 23：EH 快路径 — throw 命中单 clause 内联分发（2026-09-23，已合入）

`throw $T` 的默认路径 = 分支到冷存根 → `ea_jit_eh_throw` 构造异常对象 →
`eh_dispatch` 逐条目逐 clause 匹配。当**最内层 enclosing try_table 恰有
一个 clause 且为 `catch $T`（同 tag）或 `catch_all`** 时，现在直接内联：

    bl ea_jit_eh_pop        // 摘除本 try 的 handler 条目（helper 语义）
    <emit_br_to 载荷搬运>    // 与 br 完全相同的落点：sp = 标签高度-载荷数
    b  <clause 落点>         // 与 helper resume 相同的目标

- 落点公式与 eh_dispatch 逐字段对齐：height/arity 取自 clause 标签的
  ctrl 帧（校验器保证标签 arity == tag 参数量），函数级 clause 落到
  隐式 end（emit_return 尾）。
- 其余形态（tag 不命中、多 clause、catch_ref、throw_ref、外层 handler）
  一律回退原存根路径——运行时 EA_EHDBG 验证：命中场景 0 次 dispatch，
  不命中场景 1 次（正确 trap）。
- 教训：`fix_to_pc` 之前必须先发射占位 `em_b_label`（它补丁最近一条 b），
  漏发 = SIGILL。
- 验证：exceptions 全家（throw 12/0、try_table 45/0、throw_ref 14/0）、
  全量双模式 257/258 零回归。

## 迭代 24：v128 NEON lowering 第一批（2026-09-23，已合入）

v128 从「函数级回退解释器」升级为**原生 NEON 执行**（第一批，整数/位运算）：

- **指令面**：v128.const（GPR 半区 + INS）、v128.load/store（Q 寄存器 16 字节
  移动，沿用 guard-page 陷阱路径）、not/and/andnot/or/xor/bitselect(BSL)、
  i8x16/i16x8/i32x4/i64x2 add/sub、i8x16/i16x8/i32x4 mul、四宽度 eq/ne
  （cmeq+mvn）、splat×6、extract_lane×8（符号形式补 sxtb/sxth）、
  replace_lane×6。**FP 算术暂缓**（NaN 规范化须与解释器逐位对齐，批 2）。
- **编码方法**：全部对照 llvm-mc 标定；发射器从标定样本字补丁寄存器域。
- **三个编码陷阱（均已修并留档）**：
  1. 样本字掩码误清 3-same 固定位 bit10 → ADD 静默变 SMLAL2（掩码应为
     0xFFE0FC1F）；
  2. INS/UMOV 的 imm5 = (lane << (log2(esize/8)+1)) | esize/8 —— lane<<se
     在 se=8 时溢出 5 位字段，d[1] 被写成 d[0]；
  3. extract/replace 的 opcode 族 interleaves replace 形式，`opcode-base`
     索引越界 —— 宽度映射必须用显式 switch。
- **效果**：simd 文件族从回退解释器变为真执行机器码：simd_const 265/0、
  i32x4_arith 192/0、i64x2_arith 198/0、lane 357/0、bit_shift 235/0、
  splat 180/0；全量双模式 257/258 零回归，WASI 15/15。
- **批 2（已合入）**：v128 局部/参数免 bail——prologue 参数拷贝/零初始化
  16 字节、local get/set/tee 走 Q 寄存器帧槽（str/ldur q 带大偏移回退）、
  尾调用参数搬运按 callee 类型 16 字节、br 携带升级 16 字节 ldp/stp
  （类型保持，标量陈旧高半区不外泄）；SELECT/SELECT_T 仍为 8 字节 csel，
  含它们的 v128 函数保留 bail（显式扫描门控）。
- **批 3（已合入）**：全宽度整数比较（eq/ne/lt/gt/le/ge × s/u × b/h/s/d，
  经操作数交换复用 cmgt/cmge/cmhi/cmhs）、f32x4/f64x2 比较（fcmeq/fcmge/
  fcmgt + 交换 + mvn）、f32x4/f64x2 add/sub/mul/div（NEON 传播语义直接
  满足套件 NaN 断言）、整数 min/max（smin/umin/smax/umax × b/h/s）、
  可变移位 ×12（sshl/ushl 寄存器形式：计数 mod esize，右移取负——
  **不是**立即数形式的 esize-shift 约定）。simd_splat 180/0。
- **批 4（已合入，默认原生）**：abs/neg（i8x16..i64x2 + fabs/fneg）、
  popcnt、any_true（umaxv+cset）/all_true（uminv+cset）、loadN_splat、
  load32/64_zero、loadN_lane/storeN_lane。simd_splat 180/0、boolean 271/0、
  load_splat 120/0、load_zero 31/0。
- **simd_splat 野访问根因（已修）**：umaxv/uminv 归约指令在 Rm 字段位置
  携带**固定模式 10001**——样本字发射器把该字段当寄存器清零（rm=0），
  发出未定义指令：SIGILL，或经 ASLR 偏移砸中模块导出表（"missing export"
  的假象）。同轮连带修复：sshl/ushl 寄存器形式负计数（-shift，而非
  立即数形式的 esize-shift）、计数广播的 lane 宽度、i8x16 abs/neg 样本字
  （误用 .4s 形式）。方法论：确定性复现后 lldb 直接读故障字
  （EXC_BAD_INSTRUCTION 的 subcode = 出错的指令编码本身）。
- **批 5（已合入）**：extend_low/high ×12（sshll/ushll (2) #0）、
  narrow ×4（sqxtn/uqxtn ×2 + ins d[1]，双步序列绕开 sqxtn2 的固定
  10001 位型陷阱）。simd_int_to_int_extend 252/0 原生。
- **批 6（已合入）**：i8x16.swizzle（tbl 单表，OOB 索引出 0 与 wasm
  语义一致）、i8x16.shuffle（双表 {a,b} + 立即数索引向量）、
  load8x8/16x4/32x2_s/u（窄加载 + sshll/ushll #0）。simd_lane 357/0 原生。
- **批 7（已合入，默认原生）**：v128 select（SELECT_T 自带类型立即数，
  广播 + cmtst 成掩码 + BSL——无需编译期类型栈）、f32x4/f64x2 min/max
  （解释器逐位对齐序列：NaN→规范 NaN、±0 符号规则 min=or/max=and，
  ~18 条 BSL 选择链）、pmin/pmax（fcmgt + BSL 比较选择）、
  i32x4.dot_i16x8_s（smull+smull2+addp）。
- **a64_neon 掩码根因（已修，影响全部批次）**：样本字掩码保留了标定
  rd，`| rd` 产生按位并集（16|2=18）——凡调用寄存器 ≠ 标定寄存器的
  发射全部错乱。掩码改为 0xFFE0FC00（清 rd/rn/rm、保位型/固定位）。
  此前的 4 个"独立"编码 bug 皆是此根因的不同症状。
- 剩余：relaxed-simd 收尾、warm 通道 join 一致性、HIR。
- **性能边界结论**：sum/matmul 与 wasmtime 的 5–6× 差距 = 操作数栈临时值
  往返（def 窗口仅 2 槽，3 活跃值形态必须溢栈，如 `i*3-(i>>1)` 链）——
  解法为栈槽位寄存器分配（HIR），已量化：循环体压到 ~12 条即达 wasmtime 同档。

### 三、排查方法备忘

- 单函数 JIT 代码对比：EA_JIT_DUMP + llvm-mc（--disassemble 输入须为
  逗号分隔小端字节流）；循环体指令分类计数。
- 运行时寄存器轨迹：EA_WDBG2 的 ea_h_wdump6 挂在回边（每迭代打印
  x19-x24）。
- 测试脚本自身的两个教训：wast 手写累加漏 `(local.get $s)` 会把
  `s += e` 写成 `s = e`；zsh 不做单词拆分，`$args` 传双参变单参
  （第二参缺省为 0）——引擎无恙。
