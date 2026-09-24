# easm — embed WebAssembly

面向 aarch64（Apple Silicon 等）的嵌入式 WebAssembly 运行时：以**高效 1:1 指令级翻译的
baseline JIT** 为主执行引擎，不能降级的指令形态自动回退解释器。定位是嵌入式场景——
小体积（可执行文件 ~240KB）、低内存（基线 RSS ~1.7MB）、快启动（毫秒级）。

## 特性

- **解码器**：LEB128 / 全 section，覆盖 Wasm 2.0/3.0 特性位（bulk-memory、
  reference-types、multi-memory、table64、extended-const、SIMD、GC、exceptions 等）
- **校验器**：类型栈机 + 控制帧，unreachable 多态；GC 类型系统——`rec`/`sub`
  声明式子类型、iso-recursive **canonical 等价**（组内位置引用 vs 外部引用，
  归纳式组对比较，跨模块 import 解析）
- **解释器**：值栈 + 控制帧，`setjmp/longjmp` trap 传播
- **aarch64 baseline JIT**：1:1 指令级翻译（操作数栈即机器栈），浮点直通 S/D 寄存器、
  延迟操作数融合（单值/双值窗口）、真尾调用（`br` 尾跳，C 栈零增长）、br_table、
  bulk-memory / table 指令
- **原生 SIMD（NEON）**：v128 常量/加载存储、整数 add/sub/mul、全宽度比较、
  min/max、可变移位、splat/extract/replace lane、位运算与 bitselect 直接降级为
  NEON 指令；v128 局部/参数按 16 字节槽原生承载，剩余形态自动回退解释器
- **JIT exception handling**：`try_table`/`throw`/`throw_ref` 原生代码生成——
  handler 栈逐帧 marker 链式传播，catch 载荷按 `br` 语义精确落栈，
  JIT↔解释器边界双向传播；`throw` 命中单 clause try_table 时内联分发
  （省去存根+helper 走栈）；prologue 原生栈深检查（深递归 trap 而非段错误，
  leaf 函数免检）
- **GC 运行时**：struct/array 分配、i31 打包引用、`ref.test`/`ref.cast`/
  `br_on_cast` 运行时子类型检查、extern/any 侧互转
- **exceptions**：`throw`/`throw_ref`/`try_table`（catch / catch_ref / catch_all /
  catch_all_ref），标签跨实例同一性，异常沿 JIT↔解释器边界传播
- **运行时**：模块实例化、跨模块链接（`spectest` 内置）、多实例共享内存
  （任一别名实例 `memory.grow` 全局可见）、导入全局/内存别名共享存储、
  12GiB 地址空间预留的线性内存、1:1 翻译的 trap 传播
- **信号式 OOB**：32 位线性内存的 JIT 访存零边界检查指令——越界落入
  PROT_NONE 保留区，由 SIGSEGV/SIGBUS handler 转换为 wasm trap
  （wasmtime 同款机制）；64 位内存保留显式检查
- **WASI Preview 1**：21 syscall（args/env/clock/random/fd_write/read/
  seek/close/fdstat/filestat/prestat/path_open/sync/sched_yield/
  poll_oneoff/proc_exit），三平台移植层（POSIX + Win32 + ewokos HAL）；
  含 CLI demo（argv echo + 文件读写）和 GUI demo（Mandelbrot 帧缓冲
  + AppKit NSWindow）

## 目录结构

```
src/            运行时源码（C11 + ObjC，~13.5k 行）
  decode.c        二进制解码（含 GC/exceptions 指令与 rec/sub 类型）
  validate.c      校验器 + canonical 类型等价/子类型
  runtime.c       store / 实例化 / invoke
  interp.c        解释器（含 GC/异常执行）
  jit_a64.c       aarch64 baseline JIT（含 JIT↔解释器桥）
  a64_emit.h      ARM64 指令编码器（63 条编码用例对照 llvm-mc 全对）
  arith.c         数值指令
  simd_exec.c     v128 指令（解释器执行）
  wasi.c          WASI Preview 1 核心（21 syscall，fd 表 + 路径解析）
  wasi_posix.c    POSIX 后端（macOS / Linux）
  wasi_win.c      Win32 后端（Windows，UTF-16 宽 API）
  wasi_ewok.c     ewokos 后端（HAL vtable + RAM 文件系统）
  wasi.h / wasi_platform.h  WASI 接口与平台抽象层
  easm_gui.m      AppKit GUI 查看器（Objective-C）
  wast.c          wast2json 测试驱动（含 module definition / instance 命令）
  main.c          CLI（run / wasi / wast 命令）
tools/          run_spec.py（覆盖率）、run_bench.py（基准）、wast_convert.py
                （wast2json 失败时的自制转换兜底）
tests/          官方 spec 测试集快照 + 提案测试 + WASI 测试集
bench/          基准内核（freestanding wasm32）
PROGRESS.md     阶段报告：优化迭代记录、与 wasmtime 的对比数据
```

## 构建

```bash
make -j8          # 产物 build/easm（约 260KB）
make clean
```

依赖：clang（Apple Silicon 自带）、macOS（JIT 使用 `MAP_JIT` +
`pthread_jit_write_protect_np`）。GUI 查看器构建：

```bash
clang -x objective-c src/easm_gui.m src/decode.c src/validate.c \
  src/runtime.c src/interp.c src/arith.c src/util.c \
  src/wasi.c src/wasi_posix.c src/wasi_ewok.c \
  -framework AppKit -framework Foundation -ldl -O1 -o build/easm_gui
```

基准内核的重建另需 wabt 与 LLVM：

```bash
/opt/homebrew/opt/llvm/bin/clang --target=wasm32 -O2 -nostdlib \
  -Wl,--no-entry -Wl,--initial-memory=2097152 -o bench/bench.wasm bench/bench.c
```

## 使用

```bash
# 运行模块导出（自动按函数签名解析参数）
build/easm run bench/bench.wasm primes 1000
EA_JIT=1 build/easm run bench/bench.wasm fib 47      # 启用 JIT

# 运行 wast2json 转换的测试文件
build/easm wast path/to/case.json [-v]

# 环境变量
EA_JIT=1         启用 JIT（默认解释器）
EA_JIT_STATS=1   编译/调用统计（bail 原因、尾调用追踪）
EA_JIT_DUMP=1    dump JIT 机器码（可用 llvm-mc --disassemble 反汇编阅读）
EA_JIT_TRACE=1   跟踪 JIT 执行
```

## 测试与基准

```bash
python3 tools/run_spec.py -j 8          # 解释器模式一致性覆盖率
python3 tools/run_spec.py -j 8 --jit    # JIT 模式（每文件真正跑 JIT 机器码）
python3 tools/run_bench.py all          # JIT / 解释器 / wasmtime / node 对比
```

### 当前状态（2026-09-23，详见 [PROGRESS.md](PROGRESS.md)）

**覆盖率**（官方 spec 套件，258 个 wast 文件，双模式）：

- 解释器模式：**257 个文件全过（99.6%）**，累计 61,120+ 条 assert
- **JIT 模式：257/258 全过，与解释器逐文件一致**——每个文件的 JIT 编译函数
  实际执行机器码（含 exceptions / multi-memory / 原生 NEON SIMD），
  61,124 条 assert 全部通过
- 唯一双模式都未过：annotations.wast（wabt 与 wasm-tools 均不支持注解提案
  文本语法，转换失败，非运行时问题）
- 已覆盖：Wasm 2.0 全部 + GC（struct/array/i31/ref.test/cast/br_on_cast/rec/sub）、
  typed function references、**exceptions（throw/throw_ref/try_table 含 JIT）**、
  multi-memory、table64、**SIMD + relaxed-simd（全家族原生 NEON，含
  fmla/sdot/tbl 融合）**

**性能**（同机与 wasmtime 48.0.2 / node 对比，含引擎启动）：

| kernel | easm-jit | easm-int | wasmtime | node |
|---|---|---|---|---|
| fib（10M 迭代） | **0.006** | 0.438 | 0.008 | 0.028 |
| primes（500K 筛） | 0.006 | 0.307 | 0.005 | 0.026 |
| sum（100M i64） | **0.017** | 8.739 | 0.019 | 0.075 |
| matmul（30×128³） | 0.062 | 16.475 | 0.030 | 0.088 |
| memsum（2M load） | **0.003** | 0.007 | 0.005 | 0.034 |

- **fib/sum/memsum 反超 wasmtime**，primes 追平（±20% 内波动），matmul
  2.1×；五内核几何平均约 0.9×——**总体追平 wasmtime**
- 关键一步：warm 两遍编译只对**叶子循环**（体内无嵌套 loop）触发，
  want 集精确覆盖热循环（matmul 内环六值全进缓存槽）；FP/vector
  push 不再驱逐缓存引用（n_fpush 层序计数，pop 按 LIFO 排干），tee
  的 in_place 结果以零代码缓存引用入栈，FP 双目的 parked 操作数与
  cref 统一经 pop_s/d 消费
- JIT 相对解释器加速 **14–2600×**；sum 自迭代 24 的 0.108 收敛至
  0.017（-84%）
- 逃生门：EA_NOCACHE / EA_NOWARM 恢复纯窗口直发语义（仍 257/258）

**内存**（峰值 RSS）：easm 基线 1.7MB、负载峰值 2.2MB，约为 wasmtime 的 1/3.6、
V8/node 的 1/20；磁盘足迹 313KB vs wasmtime 46MB；冷启动（解码+编译+执行
sum(1)）**2.3ms，快于 wasmtime 的 4.1ms**。

## 路线图

1. matmul 剩余差距（2.1×）——内环 FP 值的 v 寄存器直驻（跳过 GPR 位
   模式往返）与访存合并
2. annotations 提案文本语法支持（自制转换路径，解锁最后一个测试文件）

## WASI 运行时

三平台移植层通过 `EaWasiPlat` vtable 抽象，宿主集成者只需实现平台回调：

| 后端 | 目标平台 | 文件系统 | 时钟 | 随机 | 验证 |
|---|---|---|---|---|---|
| POSIX | macOS / Linux | open/read/write/seek/mkdir/unlink | clock_gettime | /dev/urandom | ✓ 全功能 |
| Win32 | Windows | CreateFileW/ReadFile（UTF-16 双宽） | QueryPerformanceCounter | rand() | mingw 编译 ✓ |
| ewokos | 嵌入式 RTOS | RAM-fs（64KB/文件，flat 目录） | 1ms fake clock | LCG | ✓ 全功能 |

```bash
# CLI demo（POSIX 后端）
build/easm wasi demos/wasi_echo.wasm hello easm-world

# 文件读写（指定 preopen 目录）
build/easm wasi --dir d=demos demos/wasi_file.wasm

# ewokos 嵌入式后端（RAM 文件系统）
EA_BACKEND=ewok build/easm wasi demos/wasi_echo.wasm ewok-hello

# GUI demo（AppKit Mandelbrot 窗口）
build/easm_gui demos/wasi_gui.wasm

# 验证矩阵（15 项：3 后端 × echo/file + GUI）
bash tools/test_wasi.sh
```

**验证结果**：15/15 通过（POSIX echo 5 项 + EWOK echo 2 项 +
POSIX file 4 项 + EWOK file 3 项 + GUI 1 项），覆盖 argv 传递、
clock 读取、path_open 创建/写入/关闭/重开/读回/内容校验、GUI 帧渲染。

## License

[MIT](LICENSE)
