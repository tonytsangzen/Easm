# WebAssembly 标准测试用例集

本目录收集 WebAssembly 标准及活跃提案的官方测试用例，用于自研 WASM 运行时的 conformance 验收。
所有用例均来自 `github.com/WebAssembly` 组织下的官方仓库（main 分支快照）。

## 目录结构

```
tests/
├── spec/                  # 官方 spec 主仓库（已标准化特性的权威测试）
├── wasi-testsuite/        # WASI 系统调用层测试（源码形式，需构建）
└── proposals/             # 未合并进主 spec 的活跃提案测试
    ├── meta/              # 提案阶段索引（来自 WebAssembly/proposals）
    ├── threads/
    ├── stack-switching/
    ├── custom-page-sizes/
    ├── shared-everything-threads/
    ├── jit-interface/
    ├── js-promise-integration/
    ├── wide-arithmetic/
    ├── compact-import-section/
    ├── component-model/
    └── stringref/
```

## 用例统计

| 套件 | 路径 | 用例数 | 说明 |
|---|---|---:|---|
| 核心 spec | `spec/test/core/*.wast` | **258** | 指令语义、验证、trap、GC、SIMD、relaxed-SIMD、异常、memory64、multi-memory、bulk-memory、tail-call、reference-types 等已标准化特性 |
| JS API | `spec/test/js-api/*.js` | 61 | Web JS API 绑定测试（运行时可不跑，参考用） |
| wasi-testsuite | `wasi-testsuite/tests/{c,rust,assemblyscript}/` | 240+ 源文件 | 系统调用层（fd_*、args_*、environ_*、random_get、fs、threads 等），含 `.json` 期望输出 |
| threads | `proposals/threads/test/core/` | 160 | 共享内存 + Atomics（Phase 4，即将标准化） |
| stack-switching | `proposals/stack-switching/test/core/` | 243 | 协程/异步 continuation（Phase 3） |
| custom-page-sizes | `proposals/custom-page-sizes/test/core/` | 262 | 自定义内存页大小（Phase 3） |
| shared-everything-threads | `proposals/shared-everything-threads/test/core/` | 158 | 共享-everything 线程模型（Phase 1，未来方向） |
| jit-interface | `proposals/jit-interface/test/core/` | 258 | JIT 编译接口提案（Phase 1，直接关系运行时 JIT 设计） |
| js-promise-integration | `proposals/js-promise-integration/test/core/` | 227 | JS Promise 集成（Phase 5，即将标准化） |
| wide-arithmetic | `proposals/wide-arithmetic/test/core/` | 259 | 宽整数算术（i128 等，Phase 4） |
| compact-import-section | `proposals/compact-import-section/test/core/` | 260 | 紧凑 import section 编码（Phase 4） |
| component-model | `proposals/component-model/test/` | 72 (.wast) | 组件模型（async/binary/values/validation/linking/resources） |
| stringref | `proposals/stringref/test/core/` | 145 | 引用类型字符串（Phase 1） |

> **说明**：proposals 仓库的 `test/core/` 数字包含提案时的 spec 基线副本，实际"该提案新增"的用例远少于此。自研运行时建议只把每个 proposal 相对 spec 基线新增/修改的 .wast 作为该特性的增量验收。

## 运行方式

### 1. 跑 spec core .wast（最权威的 conformance 验收）

`.wast` 是 wast 脚本格式，包含 `module` / `assert_return` / `assert_trap` / `assert_invalid` 指令。
自研运行时需实现一个 wast 解释器/driver，逐文件加载、执行、断言。参考实现：

- `spec/test/run.py`（官方 Python 驱动，调用 wasm-spec-interpreter）
- Wasmtime: `cargo run --manifest-path crates/wasmtime/Cargo.toml -- --test-spec core`
- WAMR: `./wamr-test-suites/wamr_test_runner.py`
- V8: `tools/run-tests.py --expose-gc mjsunit/wasm`

最小 driver 流程：
1. 解析 wast → 多个二进制模块 + 命令式断言；
2. 编译/实例化每个模块；
3. 执行导出函数，对比 `assert_return` 的栈上结果；
4. 检查 `assert_trap` / `assert_invalid` 的错误码。

### 2. 跑 wasi-testsuite

仓库里是**源码**（C / Rust / AssemblyScript），需要先构建成 `.wasm`：

```bash
# C 测试：用 wasi-sdk 编译
cd wasi-testsuite/tests/c
# 每个 .c → .wasm32-wasip1

# Rust 测试：
cd wasi-testsuite/tests/rust/wasm32-wasip1
cargo build --target wasm32-wasip1 --release

# 运行：
cd wasi-testsuite/test-runner
pip install -r requirements.txt
python -m wasi_test_runner --runtime <your-wasm-runner>
```

期望输出在 `wasi-testsuite/tests/**/*.json`（stdout/exit_code/ stderr 正则），
以及 `wasi-testsuite/expectations/{wazero,wasmtime,wamr,jco}/` 下的 per-engine 容差表。

### 3. 跑 proposals

每个 proposal 仓库的 `test/core/` 与 spec 格式相同，直接复用同一个 wast driver，
按提案开关开启对应特性即可（例如跑 `stack-switching` 时打开 `--experimental-stack-switching`）。

## 与自研运行时验收的对应关系

| 自研里程碑 | 必须通过的用例集 |
|---|---|
| MVP：解释器 + 基本数值/控制流 | `spec/test/core/{i32,i64,f32,f64,int_exprs,float_*,block,loop,if,br*,call,return,select,load,store,memory,table,global}.wast` |
| 校验器 MVP | `spec/test/core/binary*.wast`、`linking.wast`、`invalid` 类断言 |
| Baseline JIT | 上述全部 + `spec/test/core/fac.wast`、`func_ptrs.wast`、`call_indirect.wast` |
| 优化 JIT / AOT | 全量 `spec/test/core/`（含 gc/、simd/、bulk-memory/ 子目录） |
| 多线程 | `proposals/threads/test/core/` + `wasi-testsuite` 的 threads 用例 |
| 协程/异步 | `proposals/stack-switching/test/core/` |
| 系统调用层 | `wasi-testsuite/tests/rust/wasm32-wasip1/` 全量 |
| 组件模型 | `proposals/component-model/test/` |
| JIT 接口（自研反射/动态编译） | `proposals/jit-interface/test/core/` |

## 来源

- 主 spec: https://github.com/WebAssembly/spec （main 分支快照，2026-09-21）
- WASI 测试: https://github.com/WebAssembly/wasi-testsuite
- 提案元索引: https://github.com/WebAssembly/proposals
- 各提案仓库见 `proposals/<name>/README.md`
