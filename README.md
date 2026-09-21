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
  bulk-memory / table 指令；v128 等罕见形态自动回退解释器
- **GC 运行时**：struct/array 分配、i31 打包引用、`ref.test`/`ref.cast`/
  `br_on_cast` 运行时子类型检查、extern/any 侧互转
- **exceptions**：`throw`/`throw_ref`/`try_table`（catch / catch_ref / catch_all /
  catch_all_ref），标签跨实例同一性，异常沿 JIT↔解释器边界传播
- **运行时**：模块实例化、跨模块链接（`spectest` 内置）、多实例共享内存
  （任一别名实例 `memory.grow` 全局可见）、导入全局/内存别名共享存储、
  12GiB 地址空间预留的线性内存、1:1 翻译的 trap 传播

## 目录结构

```
src/            运行时源码（C11，~11.5k 行）
  decode.c        二进制解码（含 GC/exceptions 指令与 rec/sub 类型）
  validate.c      校验器 + canonical 类型等价/子类型
  runtime.c       store / 实例化 / invoke
  interp.c        解释器（含 GC/异常执行）
  jit_a64.c       aarch64 baseline JIT（含 JIT↔解释器桥）
  a64_emit.h      ARM64 指令编码器（63 条编码用例对照 llvm-mc 全对）
  arith.c         数值指令
  simd_exec.c     v128 指令（解释器执行）
  wast.c          wast2json 测试驱动（含 module definition / instance 命令）
  main.c          CLI
tools/          run_spec.py（覆盖率）、run_bench.py（基准）、wast_convert.py
                （wast2json 失败时的自制转换兜底）
tests/          官方 spec 测试集快照 + 提案测试 + WASI 测试集
bench/          基准内核（freestanding wasm32）
PROGRESS.md     阶段报告：优化迭代记录、与 wasmtime 的对比数据
```

## 构建

```bash
make -j8          # 产物 build/easm（约 240KB）
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

- **257 个文件全过（99.6%）**，累计 61,110 条 assert；JIT 模式与解释器模式逐文件一致
- 唯一未过：annotations.wast（wabt 与 wasm-tools 均不支持注解提案文本语法，
  转换失败，非运行时问题）
- 已覆盖：Wasm 2.0 全部 + GC（struct/array/i31/ref.test/cast/br_on_cast/rec/sub）、
  typed function references、exceptions（throw/throw_ref/try_table）、
  multi-memory、table64、SIMD（非 relaxed）

**性能**（同机与 wasmtime 48.0.2 / node 对比，含引擎启动）：

| kernel | easm-jit | easm-int | wasmtime | node |
|---|---|---|---|---|
| fib（10M 迭代） | 0.022 | 0.459 | 0.009 | 0.031 |
| primes（500K 筛） | **0.006** | 0.318 | 0.006 | 0.028 |
| sum（100M i64） | 0.109 | 9.221 | 0.021 | 0.080 |
| matmul（30×128³） | 0.340 | 17.416 | 0.031 | 0.091 |
| memsum（2M load） | **0.003** | 0.007 | 0.005 | 0.026 |

- primes **追平 wasmtime**，memsum 快于 wasmtime；fib/primes 快于 node
- JIT 相对解释器加速 **17–48×**；计算密集内核剩余差距来自跨语句值生命周期
  （地址算术、f32 load/store 链）→ HIR/寄存器分配阶段的收益空间

**内存**（峰值 RSS）：easm 基线 1.7MB、负载峰值 2.2MB，约为 wasmtime 的 1/3.6、
V8/node 的 1/20；磁盘足迹 ~240KB vs wasmtime 48.5MB。

## 路线图

1. HIR + 寄存器分配（效率阶段主战场）——消除 1:1 栈机翻译的冗余 push/pop，
   收敛与 Cranelift 在计算密集内核上的差距；顺带把 call_indirect 类型检查
   改为解码期缓存 canonical id
2. 浮点参数直接 S/D 寄存器往返；v128 的 JIT lowering（当前回退解释器）
3. annotations 提案文本语法支持（自制转换路径，解锁最后一个测试文件）

## License

[MIT](LICENSE)
