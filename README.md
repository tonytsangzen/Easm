# easm — embed WebAssembly

面向 aarch64（Apple Silicon 等）的嵌入式 WebAssembly 运行时：以**高效 1:1 指令级翻译的
baseline JIT** 为主执行引擎，不能降级的指令形态自动回退解释器。定位是嵌入式场景——
小体积（可执行文件 <200KB）、低内存（基线 RSS ~1.7MB）、快启动（毫秒级）。

## 特性

- **解码器**：LEB128 / 全 section，覆盖 Wasm 2.0/3.0 特性位（bulk-memory、reference-types、
  multi-memory、table64、extended-const、SIMD 等）
- **校验器**：类型栈机 + 控制帧，unreachable 多态，返回类型一致性检查（含 return_call）
- **解释器**：值栈 + 控制帧，`setjmp/longjmp` trap 传播
- **aarch64 baseline JIT**：1:1 指令级翻译（操作数栈即机器栈），浮点直通 S/D 寄存器、
  延迟操作数融合（单值/双值窗口）、真尾调用（`br` 尾跳，C 栈零增长）、br_table、
  bulk-memory / table 指令；v128 等罕见形态自动回退解释器
- **运行时**：模块实例化、跨模块链接（`spectest` 内置）、12GiB 地址空间预留的线性内存、
  1:1 翻译的 trap 传播

## 目录结构

```
src/            运行时源码（C11，~9k 行）
  decode.c        二进制解码
  validate.c      校验器
  runtime.c       store / 实例化 / invoke
  interp.c        解释器
  jit_a64.c       aarch64 baseline JIT（含 JIT↔解释器桥）
  a64_emit.h      ARM64 指令编码器（63 条编码用例对照 llvm-mc 全对）
  arith.c         数值指令
  simd_exec.c     v128 指令（解释器执行）
  wast.c          wast2json 测试驱动
  main.c          CLI
tools/          run_spec.py（覆盖率）、run_bench.py（基准）
tests/          官方 spec 测试集快照 + 提案测试 + WASI 测试集
bench/          基准内核（freestanding wasm32）
PROGRESS.md     阶段报告：优化迭代记录、与 wasmtime 的对比数据
```

## 构建

```bash
make -j8          # 产物 build/easm（约 190KB）
make clean
```

依赖：clang（Apple Silicon 自带）、macOS（JIT 使用 `MAP_JIT` +
`pthread_jit_write_protect_np`）。基准内核的重建另需 wabt 与 LLVM：

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
python3 tools/run_spec.py -j 8 --jit    # JIT 模式（逐文件结果与解释器一致）
python3 tools/run_bench.py all          # JIT / 解释器 / wasmtime / node 对比
```

### 当前状态（2026-09-21，详见 [PROGRESS.md](PROGRESS.md)）

**覆盖率**（官方 spec 套件，258 个 wast 文件）：

- 170 个文件全过（65.9%），累计 57,483 条 assert；JIT 模式与解释器模式逐文件一致
- 未过文件集中在二阶段特性：GC/sub/rec 类型语法、function-references/call_ref、
  relaxed-simd、exceptions 等

**性能**（同机与 wasmtime 48.0.2 / node 对比，含引擎启动）：

| kernel | easm-jit | easm-int | wasmtime | node |
|---|---|---|---|---|
| fib（10M 迭代） | 0.023 | 0.446 | 0.008 | 0.032 |
| primes（500K 筛） | 0.009 | 0.308 | 0.005 | 0.026 |
| sum（100M i64） | 0.181 | 8.985 | 0.020 | 0.079 |
| matmul（30×128³） | 0.351 | 17.390 | 0.029 | 0.089 |
| memsum（2M load） | **0.003** | 0.007 | 0.005 | 0.024 |

- JIT 相对解释器加速 **17–49×**；fib/primes 快于 node，memsum 快于 wasmtime
- 计算密集内核与 wasmtime 差 9–12×（Cranelift 寄存器分配/向量化 → HIR 阶段收益空间）

**内存**（峰值 RSS）：easm 基线 1.7MB、负载峰值 2.2MB，约为 wasmtime 的 1/3.6、
V8/node 的 1/20；磁盘足迹 189KB vs wasmtime 48.5MB。

## 路线图

1. GC 类型语法（`rec`/`sub`/struct）最小编解码 —— 解开 return_call/call_ref 等
   6+ 个文件的剩余失败
2. HIR + 寄存器分配 —— 收敛与 Cranelift 在计算密集内核上的差距
3. v128 的 JIT lowering（当前回退解释器）
4. function-references / exceptions / 多返回值等二阶段特性

## License

[MIT](LICENSE)
