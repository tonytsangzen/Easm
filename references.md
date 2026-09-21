# 参考资料清单

> 合并自三份研究稿（`01_wasm_runtimes.md` / `02_wasm_proposals_and_jit_classics.md` / `03_rosetta_and_dbt.md`），去重后按主题分组编号。编号在 HTML 报告与飞书文档中以 `[n]` 上标引用。
>
> 来源类型：**官方 spec** / **官方博客** / **官方文档** / **社区逆向** / **学术论文** / **社区博客**。

| 编号 | 标题 | URL | 一句话用途 | 来源类型 |
|---|---|---|---|---|
| 1 | Wasm Core Spec — Modules | https://webassembly.github.io/spec/core/syntax/modules.html | 模块结构与 section 组织的官方定义 | 官方 spec |
| 2 | W3C WebAssembly Core Specification 1.0 | https://www.w3.org/TR/wasm-core-1/ | 校验（validation）与类型安全承诺的规范原文 | 官方 spec |
| 3 | WebAssembly JS Interface | https://webassembly.github.io/spec/js-api/ | 浏览器侧 embedder API | 官方 spec |
| 4 | WebAssembly 2.0 Core Specification PDF | https://webassembly.github.io/custom-descriptors/versions/core/WebAssembly-2.0.pdf | Wasm 2.0 正式文本（含 reference types 语义） | 官方 spec |
| 5 | Wasm 3.0 Completed (2025-09-17) | https://webassembly.org/news/2025-09-17-wasm-3.0/ | Wasm 3.0 发布时间锚点 | 官方博客 |
| 6 | WebAssembly finished-proposals.md | https://github.com/WebAssembly/proposals/blob/main/finished-proposals.md | 已标准化提案清单 | 官方 spec |
| 7 | WebAssembly proposals README | https://github.com/WebAssembly/proposals/blob/main/README.md | 活跃提案与 phase 状态 | 官方 spec |
| 8 | WebAssembly/gc | https://github.com/WebAssembly/gc | WasmGC 提案仓库 | 官方 spec |
| 9 | WebAssembly/exception-handling | https://github.com/WebAssembly/exception-handling | EH 提案仓库 | 官方 spec |
| 10 | WebAssembly/tail-call | https://github.com/WebAssembly/tail-call | Tail Call 提案仓库 | 官方 spec |
| 11 | WebAssembly/simd | https://github.com/WebAssembly/simd | Fixed-width 128-bit SIMD 提案 | 官方 spec |
| 12 | WebAssembly/relaxed-simd | https://github.com/WebAssembly/relaxed-simd | Relaxed SIMD 提案 | 官方 spec |
| 13 | WebAssembly/threads | https://github.com/webassembly/threads | Threads / Atomics 提案 | 官方 spec |
| 14 | WebAssembly/memory64 | https://github.com/WebAssembly/memory64 | Memory64 提案 | 官方 spec |
| 15 | WebAssembly/function-references | https://github.com/WebAssembly/function-references | Typed funcref / call_ref 提案 | 官方 spec |
| 16 | WebAssembly/shared-everything-threads | https://github.com/WebAssembly/shared-everything-threads | 下一代共享堆多线程提案 | 官方 spec |
| 17 | WebAssembly/multi-memory | https://github.com/WebAssembly/multi-memory | Multimemory 提案 | 官方 spec |
| 18 | WebAssembly/esm-integration | https://github.com/WebAssembly/esm-integration | ESM Integration 提案 | 官方 spec |
| 19 | WebAssembly/component-model | https://github.com/WebAssembly/component-model | Component Model 提案 | 官方 spec |
| 20 | WebAssembly/stack-switching | https://github.com/WebAssembly/stack-switching | 协程 / fiber 提案 | 官方 spec |
| 21 | WebAssembly/custom-page-sizes | https://github.com/WebAssembly/custom-page-sizes | 自定义页大小提案 | 官方 spec |
| 22 | WebAssembly/wide-arithmetic | https://github.com/WebAssembly/wide-arithmetic | i128 宽算术提案 | 官方 spec |
| 23 | WebAssembly/acquire-release-atomics | https://github.com/WebAssembly/acquire-release-atomics | Acquire-Release 原子操作 | 官方 spec |
| 24 | WebAssembly/half-precision | https://github.com/WebAssembly/half-precision | FP16 提案 | 官方 spec |
| 25 | WebAssembly/compilation-hints | https://github.com/WebAssembly/compilation-hints | 编译提示（warmup 友好） | 官方 spec |
| 26 | WebAssembly/js-promise-integration | https://github.com/WebAssembly/js-promise-integration | JS Promise 集成 | 官方 spec |
| 27 | Function References bikeshed | https://webassembly.github.io/function-references/core/bikeshed/index.html | typed funcref 语法草案 | 官方 spec |
| 28 | Memory64 changes appendix | https://webassembly.github.io/memory64/core/appendix/changes.html | memory64 指令变更记录 | 官方 spec |
| 29 | Exception Handling JS API | https://webassembly.github.io/exception-handling/js-api/ | EH 的 JS 绑定 | 官方 spec |
| 30 | Wasm Core Spec — Instructions | https://webassembly.github.io/spec/core/syntax/instructions.html | 指令全集（含 call_ref 节） | 官方 spec |
| 31 | Wasm Core Spec bikeshed | https://webassembly.github.io/spec/core/bikeshed/ | Globals 与引用类型定义 | 官方 spec |
| 32 | Wasm WG Charter draft (2026) | http://w3c.github.io/charter-drafts/2026/wasm-wg-charter.html | WG 正式把 Component Model 列为交付目标 | 官方 spec |
| 33 | MDN Understanding Wasm text format | https://developer.mozilla.org/en-US/docs/WebAssembly/Guides/Understanding_the_text_format | Threads / shared memory 概览 | 官方文档 |
| 34 | MDN Using the WebAssembly JavaScript API | https://developer.mozilla.org/en-US/docs/WebAssembly/Guides/Using_the_JavaScript_API | 线性内存与实例隔离 | 官方文档 |
| 35 | Security Implications of Wasm Shared-Everything Threads | https://www.systemshardening.com/articles/wasm/wasm-shared-everything-threads-security/ | 共享堆线程的安全分析 | 社区博客 |
| 36 | V8 Blog: Liftoff | https://v8.dev/blog/liftoff | Liftoff baseline JIT 设计（出码快 10×、慢 4×） | 官方博客 |
| 37 | Bytecode Alliance: Wasmtime and Cranelift in 2023 | https://bytecodealliance.org/articles/wasmtime-and-cranelift-in-2023 | Winch baseline 与 Cranelift 路线 | 官方博客 |
| 38 | Cranelift 官方站点 | https://cranelift.dev/ | Cranelift 性能基准（比 V8 慢 2%、比 LLVM 慢 14%） | 官方博客 |
| 39 | Bytecode Alliance: Cranelift inliner | https://bytecodealliance.org/articles/inliner | 2025 新增 function inliner | 官方博客 |
| 40 | Wasmtime stability tiers | https://docs.wasmtime.dev/stability-tiers.html | Winch + Cranelift 分层定位 | 官方文档 |
| 41 | Wasmtime contributing architecture | https://docs.wasmtime.dev/contributing-architecture.html | cranelift-object AOT 输出 | 官方文档 |
| 42 | Bytecode Alliance articles | https://bytecodealliance.org/articles/ | Wasmtime AArch64 Winch 支持时间 | 官方博客 |
| 43 | WAMR GitHub 仓库 | https://github.com/bytecodealliance/wasm-micro-runtime | 四执行后端 + tier-up | 官方 spec |
| 44 | ESP Component: wasm-micro-runtime 2.2.0 | https://components.espressif.com/components/espressif/wasm-micro-runtime/versions/2.2.0~2 | WAMR AOT 格式与占用（VMCore 50K/80K） | 官方文档 |
| 45 | W3C WAMR 介绍 PDF | https://www.w3.org/2020/08/29-chinese-web/wamr.pdf | hello-world 低至 16K DRAM | 官方博客 |
| 46 | TWINE 论文 (arXiv 2103.15860) | https://arxiv.org/pdf/2103.15860.pdf | AOT runtime binary size 50 KiB 实测 | 学术论文 |
| 47 | Wasmer Runtime Features | https://docs.wasmer.io/runtime/features/ | Singlepass/Cranelift/LLVM 三后端对比 | 官方文档 |
| 48 | crates.io wasmer 7.3.0-rc.1 | https://crates.io/crates/wasmer/7.3.0-rc.1 | LLVM 后端比另两者快约 50% | 官方文档 |
| 49 | Wasmer Python Embedding 1.0 博客 | https://wasmer.io/es/posts/wasmer-python-embedding-1_0 | qjs.wasm 编译时间（102/295ms） | 官方博客 |
| 50 | Introducing Wasmer v5 | https://wasmer.io/posts/introducing-wasmer-v5 | v5 CoreMark 比 v4.4 再快约 8% | 官方博客 |
| 51 | WebKit: Assembling WebAssembly | https://webkit.org/blog/7691/webassembly/ | JSC BBQ + OMG + B3 路径 | 官方博客 |
| 52 | Introducing the B3 JIT Compiler | https://webkit.org/blog/5852/introducing-the-b3-jit-compiler/ | B3 取代 LLVM 的理由 | 官方博客 |
| 53 | Introducing JetStream 3 | https://webkit.org/blog/17899/introducing-the-jetstream-3-benchmark-suite/ | IPInt 覆盖范围与 WasmGC 内联存储 | 官方博客 |
| 54 | WebKit Features in Safari 18.2 | https://webkit.org/blog/16301/webkit-features-in-safari-18-2/ | Safari 18.2 起 WasmGC | 官方博客 |
| 55 | Bun Runtime 文档 | https://bun.com/docs/runtime | JSC 启动快于 V8 的官方表述 | 官方文档 |
| 56 | docs.rs wasmi_wast 2.0.0-beta.8 | https://docs.rs/crate/wasmi_wast/2.0.0-beta.8 | Wasmi 纯 Rust 解释器 API | 官方文档 |
| 57 | Wasmi 2.0 发布博客 | https://wasmi-labs.github.io/blog/posts/wasmi-v2.0/ | 2.0 快约 2.2×，手写汇编 dispatch | 官方博客 |
| 58 | SpiderMonkey: Is Memory64 actually worth using? | https://spidermonkey.dev/blog/2025/01/15/is-memory64-actually-worth-using.html | Memory64 实测收益不一定正向 | 官方博客 |
| 59 | WasmGC enabled by default in Chrome | https://developer.chrome.com/blog/wasmgc | Chrome 119 起 WasmGC 默认开启 | 官方博客 |
| 60 | DeepWiki: V8 Turboshaft & TurboFan | https://deepwiki.com/v8/v8/3.3-turboshaft-and-turbofan:-optimizing-compilers | Turboshaft 取代 Sea-of-Nodes | 社区博客 |
| 61 | DeepWiki: Wasmtime Cranelift Compiler | https://deepwiki.com/bytecodealliance/wasmtime/4.1-cranelift-compiler | ISLE 与 regalloc2 结构 | 社区博客 |
| 62 | Leaving the Sea of Nodes (Turboshaft 译文) | https://rosettalens.com/s/ko/leaving-the-sea-of-nodes | Wasm 全程走 Turboshaft | 社区博客 |
| 63 | ursb.me: immersive webassembly | https://ursb.me/immersive/webassembly/ | 70MB 模块 1~2s 启动 | 社区博客 |
| 64 | xenojoshua: Liftoff 中文镜像 | https://xenojoshua.com/posts/2018/08/liftoff | Liftoff 官方数据中文转述 | 社区博客 |
| 65 | josenaldo: Bun vs Node 冷启动 | https://josenaldo.com.br/codex-technomanticus-site/03-dominios/tecnologia/20---bun-como-runtime-e-toolkit-all-in-one | Bun ~8ms vs Node ~45ms | 社区博客 |
| 66 | Wasmi 2.0 2.2× 报道 | https://www.webanditnews.com/2026/09/03/wasmi-2-0-delivers-2-2x-speed-boost-for-webassembly-in-constrained-environments/ | Wasmi 2.0 性能外部确认 | 社区博客 |
| 67 | LWN: Cranelift | https://lwn.net/Articles/965369/ | 2024 寄存器分配器重写 | 社区博客 |
| 68 | Cornell veri-isle preprint | https://www.cs.cornell.edu/~avh/veri-isle-preprint.pdf | 2023 CVE 与 ISLE 可验证指令选择 | 学术论文 |
| 69 | arXiv 2606.26977: ISLE | https://arxiv.org/html/2606.26977v1 | ISLE DSL 指令选择论文 | 学术论文 |
| 70 | WASI.dev | https://wasi.dev/ | wasm-c-api 与 WASI 索引 | 官方文档 |
| 71 | Cloudflare Workers: How Workers Isolates work | https://developers.cloudflare.com/workers/reference/how-workers-works/#isolates | V8 Isolate 毫秒级、比容器快 ~100× | 官方文档 |
| 72 | Cloudflare Blog: Dynamic Workers | https://blog.cloudflare.com/dynamic-workers/ | Isolate 隔离与密度 | 官方博客 |
| 73 | How Python Workers Work | https://developers.cloudflare.com/workers/languages/python/how-python-workers-work | Wizer 线性内存快照 | 官方文档 |
| 74 | Cloudflare CPU benchmarks | https://blog.cloudflare.com/unpacking-cloudflare-workers-cpu-performance-benchmarks/ | GC 调优后 CPU +25% | 官方博客 |
| 75 | Fastly: Announcing Lucet | https://www.fastly.com/blog/announcing-lucet-fastly-native-webassembly-compiler-runtime | Lucet AOT 与 34~35µs 实例化 | 官方博客 |
| 76 | Fastly: How Lucet and Wasmtime make stronger compiler together | https://www.fastly.com/blog/how-lucet-wasmtime-make-stronger-compiler-together | Lucet 并入 Wasmtime | 官方博客 |
| 77 | nordiso: Wasm production use cases | https://www.nordiso.com/blog/webassembly-production-use-cases-performance-benchmarks | Shopify Functions 5ms 预算与 Fastly 冷启动 | 社区博客 |
| 78 | Deno Permissions 文档 | https://deno.land/manual@main/basics/permissions | 默认安全模型 | 官方文档 |
| 79 | Deno 官网 | https://deno.com/ | Deno 定位 | 官方文档 |
| 80 | Deno Sandbox 博客 | https://deno.com/blog/introducing-deno-sandbox | OS 级 defense-in-depth | 官方博客 |
| 81 | Cloudflare Rust 标签 | https://blog.cloudflare.com/tag/rust/rss | 避开 Emscripten 臃肿 | 官方博客 |
| 82 | Fastly 日文博客: 社区投资 | https://www.fastly.co.jp/jp/blog/how-fastly-and-developer-community-invest-in-webassembly-ecosystem | RLBox 沙箱 | 官方博客 |
| 83 | notist: Wasm to the browser and beyond | https://notist.co/patrickhamann/uEw4zt/webassembly-to-the-browser-and-beyond | Lucet 实例化时间转述 | 社区博客 |
| 84 | AIO APEX: Wasm beyond browser 2026 | https://aioapex.com/es/blog/webassembly-beyond-browser-2026 | Shopify 多租户实战 | 社区博客 |
| 85 | V8: Behind the Scenes (Feb 2017) | https://benediktmeurer.de/2017/03/01/v8-behind-the-scenes-february-edition/ | Ignition+TurboFan 取代 Crankshaft | 社区博客 |
| 86 | V8 Blog: Maglev | https://v8.dev/blog/maglev | 中档 SSA JIT 设计 | 官方博客 |
| 87 | V8 multi-tier pipeline | https://readoss.com/en/v8/v8/v8-multi-tier-compilation-pipeline | 四层 tier 结构 | 社区博客 |
| 88 | V8 Engine Architecture | https://sujeet.pro/articles/v8-engine-architecture | Maglev 触发 ~500 次调用 | 社区博客 |
| 89 | GitHub Blog: CVE-2023-4069 Maglev RCE | https://github.blog/2023-10-17-getting-rce-in-chrome-with-incomplete-object-initialization-in-the-maglev-compiler/ | 中档优化层安全教训 | 官方博客 |
| 90 | Oracle HotSpot Performance Enhancements | http://docs.oracle.com/en/java/javase/11/vm/java-hotspot-virtual-machine-performance-enhancements.html | TieredCompilation 5 tier | 官方文档 |
| 91 | HotSpot Escape Analysis Status | https://cr.openjdk.org/~cslucas/escape-analysis/EscapeAnalysis.html | EA 与标量替换 | 官方文档 |
| 92 | Red Hat: How JIT boosts Java | https://developers.redhat.com/articles/2021/06/23/how-jit-compiler-boosts-java-performance-openjdk | Safepoint 与 deopt 重建帧 | 官方博客 |
| 93 | GeekWorkbench: Tiered Compilation | https://geekworkbench.com/blog/technical/tiered-compilation | C1/C2 与 OSR | 社区博客 |
| 94 | JEP 544: Modern AOT Code Cache | http://openjdk.org/jeps/544 | PGO 数据持久化范例 | 官方文档 |
| 95 | Mozilla Hacks: Warp in Firefox 83 | https://hacks.mozilla.org/2020/11/warp-improved-js-performance-in-firefox-83/ | CacheIR stub 重放 | 官方博客 |
| 96 | Firefox Source Docs: How SpiderMonkey Optimizes | https://firefox-source-docs.mozilla.org/js/how-we-optimize.html | 1500 次进入 Warp | 官方文档 |
| 97 | LuaJIT Running | https://luajit.org/running.html | hotloop 56 / hotexit 10 / maxsnap 500 | 官方文档 |
| 98 | Cloudflare: getting next() out of NYI | https://blog.cloudflare.com/luajit-hacking-getting-next-out-of-the-nyi-list/ | Trace stitching | 官方博客 |
| 99 | V8 Crankshaft 概述 | http://nothingcosmos.github.io/V8Crankshaft/src/blog.html | Crankshaft 双 IR 教训 | 社区博客 |
| 100 | arXiv 2205.01183: in-place interpreter | https://arxiv.org/pdf/2205.01183v1.pdf | Wasm3/Wasmer 解释器对比 | 学术论文 |
| 101 | ACM 综述 10.1145/3731451 | https://dl.acm.org/doi/pdf/10.1145/3731451 | WAMR 寄存器化字节码 | 学术论文 |
| 102 | ACM HybridServe 10.1145/3774899.3775011 | https://dl.acm.org/doi/pdf/10.1145/3774899.3775011 | Pulley 解释器后端 | 学术论文 |
| 103 | InfoQ 2023 Wasm 年度回顾 | http://m.toutiao.com/group/7324979199884984859/ | Memory64 各引擎跟进 | 社区博客 |
| 104 | wasmparser validator.rs | https://raw.githubusercontent.com/mozilla-firefox/firefox/main/third_party/rust/wasmparser/src/validator.rs | 并行流式校验源码 | 官方 spec |
| 105 | Apple: About the Rosetta translation environment | https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment | Rosetta 官方行为与 sysctl 探测 | 官方文档 |
| 106 | Apple Security: Rosetta 2 on Apple silicon | https://support.apple.com/guide/security/rosetta-2-on-a-mac-with-apple-silicon-secebb113be1/web | AOT 落盘、supplemental signature、确定性翻译 | 官方文档 |
| 107 | Apple 支持 102527 | https://support.apple.com/en-la/102527 | `softwareupdate --install-rosetta` | 官方文档 |
| 108 | FFRI Project Champollion Part1 | https://ffri.github.io/ProjectChampollion/part1/ | oahd 进程与 /var/db/oah 布局 | 社区逆向 |
| 109 | Black Hat Asia 23: Dirty Bin Cache | https://i.blackhat.com/Asia-23/AS-23-Koh-Dirty-Bin-Cache-A-New-Code-Injection-Poisoning-Binary-Translation-Cache.pdf | AOT 缓存路径 hash 可预测性 | 社区逆向 |
| 110 | Google CTI: Rosetta2 artifacts | https://cloud.google.com/blog/topics/threat-intelligence/rosetta2-artifacts-macos-intrusions | /var/db/oah 取证证据 | 社区逆向 |
| 111 | dougallj: Why is Rosetta 2 fast? | https://dougallj.wordpress.com/2022/11/ | 近似一对一、31 GPR、FlagM/AFP/TSO、RAS 重写、1.64× 展开率 | 社区逆向 |
| 112 | Yining Karu: Porting Takua to ARM pt2 | https://blog.yiningkarlli.com/2021/07/porting-takua-to-arm-pt2 | AOT + JIT 双模式独立确认 | 社区博客 |
| 113 | DynamoRIO AArch64 port | https://dynamorio.org/page_aarch64_port.html | mangle IC/ISB 做 SMC 检测 | 官方 spec |
| 114 | AppleInsider M1 benchmarks | https://appleinsider.com/articles/20/11/17/m1-benchmarks-proves-apple-silicon-outclasses-nearly-all-current-intel-mac-chips/amp/ | Rosetta 单核损失 ~40% | 社区博客 |
| 115 | LifeinTECH M1 | https://www.lifeintech.com/2020/11/18/apple-m1/ | 单核 -26% / 多核 -21% | 社区博客 |
| 116 | Daring Fireball: M1 Macs | https://daringfireball.net/2020/11/the_m1_macs | 原生 1730/7530 vs Rosetta 1260/5600 | 社区博客 |
| 117 | cloudstreet-dev: architecture considerations | https://cloudstreet-dev.github.io/macOS-and-Unix/part12-performance/architecture-considerations.html | CPU/mem/IO/GPU 损失分档 | 社区博客 |
| 118 | Codegenes: Why QEMU can't match Rosetta | https://www.codegenes.net/blog/why-can-t-qemu-get-even-close-to-rosetta-2-s-performance-when-translating-x86-to-m1/ | 静态翻译为主 + 动态补丁为辅 | 社区博客 |
| 119 | QEMU Translator Internals (TCG) | https://www.qemu.org/docs/master/devel/tcg.html | TB 与 lookup_and_goto_ptr chaining | 官方 spec |
| 120 | QEMU TCG Intermediate Representation | https://www.qemu.org/docs/master/devel/tcg-ops.html | TEMP_EBB 与 exit_tb 语义 | 官方 spec |
| 121 | QEMU Multi-thread TCG | https://www.qemu.org/docs/master/devel/multi-thread-tcg.html | tb_set_jmp_target 原子 patch | 官方 spec |
| 122 | Bellard 2005 USENIX ATC: QEMU | https://www.usenix.org/legacy/publications/library/proceedings/usenix05/tech/freenix/full_papers/bellard/bellard_html/index.html | TB hash 表与条件跳转 chaining | 学术论文 |
| 123 | QEMU About Emulation | https://www.qemu.org/docs/master/about/emulation.html | guest/host 架构矩阵 | 官方 spec |
| 124 | PLDI 2000: Dynamo | https://dl.acm.org/doi/pdf/10.1145/358438.349303 | Trace 内联可超原生的开山论文 | 学术论文 |
| 125 | UCSD: Dynamo PLDI 2000 PDF | http://www-cse.ucsd.edu/classes/sp00/cse231/dynamopldi.pdf | Dynamo 论文本地镜像 | 学术论文 |
| 126 | MIT Tim Meng thesis: RIO | https://groups.csail.mit.edu/cag/rio/tim-meng-thesis.pdf | NEXT trace 选择算法形式化 | 学术论文 |
| 127 | DynamoRIO API: BT | https://dynamorio.org/API_BT.html | trace head 计数与 side exit | 官方 spec |
| 128 | DynamoRIO deploy | https://dynamorio.org/page_deploy.html | 部署与 flush region | 官方 spec |
| 129 | Transmeta Crusoe CMS PDF | https://www.ic72.com/pdf_file/t/431077.pdf | Code Morphing Software 三段结构 | 学术论文 |
| 130 | Berkeley CS152 L22: Virtual Machines | https://inst.eecs.berkeley.edu/~cs152/sp09/lectures/L22-VirtualMachines.pdf | Crusoe 自修改代码与 code cache | 学术论文 |
| 131 | LLVM 2003: LLVA emu | https://www.llvm.org/ProjectsWithLLVM/2003-Fall-CS497YYZ-LLVA-emu.pdf | 影子寄存器 + gated store buffer | 学术论文 |
| 132 | DankWiki: TM8300/TM8600 Efficeon | https://nick-black.com/dankwiki/images/a/a9/Tm8300tm8600.pdf | Efficeon VLIW 8 操作 | 学术论文 |
| 133 | cstopics: LuaJIT JIT techniques | https://cstopics.com/encyclopedia/compilers/just-in-time-compilation/jit-techniques/luajit | hot loop ~35 次触发 | 社区博客 |
| 134 | Talos: DBI with DynamoRIO | https://blog.talosintelligence.com/dynamic-binary-instrumentation-dbi-with-dynamorio/ | DynamoRIO 实战 | 社区博客 |
| 135 | zhenhuaw: virtual machine & dynamic compiling | https://zhenhuaw.me/blog/2019/revisiting-vitrual-machine-and-dynamic-compiling.html | DBT 综述 | 社区博客 |
