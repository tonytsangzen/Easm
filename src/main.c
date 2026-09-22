// easam CLI
#include "easm.h"
#include "wasi.h"
#include <stdio.h>
#include <string.h>

int run_wast(const char *json_path, int verbose);

static int run_wasi(int argc, char **argv) {
    // easm wasi <module.wasm> [--dir name=path] [--env K=V] [--backend ewok] [--] [guest args...]
    const char *mod = NULL;
    char *dirs[16][2]; uint32_t n_dirs = 0;
    char *envs[32]; uint32_t n_envs = 0;
    const char *backend = NULL;
    int gi = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            char *eq = strchr(argv[++i], '=');
            if (eq) { *eq = 0; dirs[n_dirs][0] = argv[i]; dirs[n_dirs][1] = eq + 1; n_dirs++; }
            else { dirs[n_dirs][0] = argv[i]; dirs[n_dirs][1] = argv[i]; n_dirs++; }
        } else if (strcmp(argv[i], "--env") == 0 && i + 1 < argc) {
            envs[n_envs++] = argv[++i];
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            gi = i + 1; break;
        } else if (!mod) {
            mod = argv[i];
        } else {
            gi = i; break;
        }
    }
    if (!mod) { fprintf(stderr, "usage: wasi <module.wasm> [--dir name=path] [--] [args...]\n"); return 2; }

    WasiCtx *ctx = ea_wasi_ctx_new(NULL, 0, envs, n_envs);
    static char *args_tab[64];
    uint32_t n_args = 0;
    args_tab[n_args++] = (char *)mod;
    for (int i = gi; i < argc && n_args < 64; i++) args_tab[n_args++] = argv[i];
    ctx->args = args_tab;
    ctx->n_args = n_args;
    setenv("EA_BACKEND_WASI", backend ? backend : "posix", 1);
    // backend selection is inside ea_wasi_init via EA_BACKEND; translate:
    if (backend) setenv("EA_BACKEND", strcmp(backend, "ewok") == 0 ? "ewok" : "posix", 1);

    EaStore *store = ea_store_new();
    char *err = NULL;
    if (ea_wasi_init(store, ctx, &err) != 0) {
        fprintf(stderr, "wasi init failed\n");
        return 1;
    }
    for (uint32_t i = 0; i < n_dirs; i++)
        ea_wasi_preopen(ctx, dirs[i][0], dirs[i][1]);

    FILE *f = fopen(mod, "rb");
    if (!f) { perror("open"); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)ea_malloc((size_t)len);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { }
    fclose(f);
    EaModule *m = NULL;
    if (ea_store_load(store, buf, (size_t)len, &m, &err) != 0) {
        fprintf(stderr, "load failed: %s\n", err ? err : "?");
        return 1;
    }
    if (getenv("EA_JIT")) ea_jit_compile_module(m);
    EaInstance *inst = NULL;
    EaTrap trap = TRAP_NONE;
    if (ea_store_instantiate(store, m, &inst, &err, &trap) != 0) {
        fprintf(stderr, "instantiate failed: %s\n", err ? err : ea_trap_msg(trap));
        return 1;
    }
    ea_wasi_bind(ctx, inst);
    int idx = ea_instance_export(inst, "_start", EAK_FUNC);
    if (idx < 0) { fprintf(stderr, "no _start export\n"); return 2; }
    (void)args_tab;
    int rc = ea_instance_invoke(store, inst, (uint32_t)idx, NULL, NULL, &trap);
    if (rc != 0 && !ctx->exited) {
        printf("trap: %s\n", ea_trap_msg(trap));
        return 1;
    }
    int code = ctx->exited ? (int)ctx->exit_code : 0;
    ea_wasi_ctx_free(ctx);
    return code;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s wast <script.json> [-v]\n", argv[0]);
        fprintf(stderr, "       %s run <module.wasm> <export> [args...]\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "wast") == 0) {
        int verbose = argc > 3 && strcmp(argv[3], "-v") == 0;
        return run_wast(argv[2], verbose);
    }
    if (strcmp(argv[1], "wasi") == 0)
        return run_wasi(argc, argv);
    if (strcmp(argv[1], "run") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: %s run <module.wasm> <export> [args...]\n", argv[0]);
            return 2;
        }
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            perror("open");
            return 2;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = (uint8_t *)ea_malloc((size_t)len);
        if (fread(buf, 1, (size_t)len, f) != (size_t)len) { /* short read */ }
        fclose(f);
        EaStore *store = ea_store_new();
        EaModule *m = NULL;
        char *err = NULL;
        if (ea_store_load(store, buf, (size_t)len, &m, &err) != 0) {
            fprintf(stderr, "load failed: %s\n", err ? err : "?");
            return 1;
        }
        if (getenv("EA_JIT")) ea_jit_compile_module(m);
        EaInstance *inst = NULL;
        EaTrap trap = TRAP_NONE;
        if (ea_store_instantiate(store, m, &inst, &err, &trap) != 0) {
            fprintf(stderr, "instantiate failed: %s\n", err ? err : ea_trap_msg(trap));
            return 1;
        }
        int idx = ea_instance_export(inst, argv[3], EAK_FUNC);
        if (idx < 0) {
            fprintf(stderr, "export not found: %s\n", argv[3]);
            return 1;
        }
        WVal args[32] = {0};
        uint32_t n_args = 0;
        for (int i = 4; i < argc && n_args < 32; i++, n_args++) {
            EaFuncType *ft = inst->funcs[idx].type;
            EaValType pt = n_args < ft->n_params ? ft->params[n_args] : VT_I32;
            if (pt == VT_F32) args[n_args].f32 = strtof(argv[i], NULL);
            else if (pt == VT_F64) args[n_args].f64 = strtod(argv[i], NULL);
            else if (pt == VT_I64) args[n_args].i64 = strtoull(argv[i], NULL, 0);
            else args[n_args].i32 = (uint32_t)strtoul(argv[i], NULL, 0);
        }
        WVal results[32] = {0};
        int rc = ea_instance_invoke(store, inst, (uint32_t)idx, args, results, &trap);
        if (rc != 0) {
            printf("trap: %s\n", ea_trap_msg(trap));
            return 1;
        }
        EaFuncType *ft = inst->funcs[idx].type;
        for (uint32_t i = 0; i < ft->n_results; i++) {
            switch (ft->results[i]) {
            case VT_I32: printf("%u\n", results[i].i32); break;
            case VT_I64: printf("%llu\n", (unsigned long long)results[i].i64); break;
            case VT_F32: printf("%g\n", (double)results[i].f32); break;
            case VT_F64: printf("%g\n", results[i].f64); break;
            default: printf("ref:%p\n", results[i].ref); break;
            }
        }
        return 0;
    }
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 2;
}
