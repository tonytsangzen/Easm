// easam CLI
#include "easm.h"
#include <stdio.h>

int run_wast(const char *json_path, int verbose);

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
