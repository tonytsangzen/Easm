// easm runtime: store, instantiation, const-expr evaluation, traps
#include "easm.h"
#include "opcodes.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sys/mman.h>

// ---------------------------------------------------------------- store
typedef struct {
    char *name;
    EaInstance *inst;
} EaNamedInst;

struct EaStore {
    EaNamedInst *named;
    uint32_t n_named, cap_named;
    EaExec exec;
};

void ea_store_set_max_depth(EaStore *s, uint32_t d) {
    s->exec.max_depth = d;
}

EaStore *ea_store_new(void) {
    EaStore *s = (EaStore *)ea_zalloc(sizeof(EaStore));
    s->exec.max_depth = 2500;
    return s;
}
void ea_store_free(EaStore *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->n_named; i++) free(s->named[i].name);
    free(s->named);
    free(s->exec.stack);
    free(s);
}
void ea_register_instance(EaStore *s, const char *name, EaInstance *inst) {
    if (s->n_named == s->cap_named) {
        s->cap_named = s->cap_named ? s->cap_named * 2 : 8;
        s->named = (EaNamedInst *)ea_realloc(s->named, s->cap_named * sizeof(EaNamedInst));
    }
    s->named[s->n_named].name = ea_strndup(name, strlen(name));
    s->named[s->n_named].inst = inst;
    s->n_named++;
}
EaInstance *ea_lookup_instance(EaStore *s, const char *name) {
    for (uint32_t i = 0; i < s->n_named; i++)
        if (strcmp(s->named[i].name, name) == 0) return s->named[i].inst;
    return NULL;
}

int ea_instance_export(EaInstance *inst, const char *name, uint8_t kind) {
    for (uint32_t i = 0; i < inst->module->n_exports; i++) {
        EaExport *ex = &inst->module->exports[i];
        if (ex->kind == kind && strcmp(ex->name, name) == 0) return (int)ex->idx;
    }
    return -1;
}

// ---------------------------------------------------------------- trap plumbing
const char *ea_store_last_trap_msg(EaStore *s) {
    return s->exec.trap_msg[0] ? s->exec.trap_msg : ea_trap_msg(s->exec.trap);
}

void ea_trap(EaExec *ex, EaTrap code) {
    if (getenv("EA_TDBG")) fprintf(stderr, "TRAP %d\n", code);
    ex->trap = code;
    ex->trap_msg[0] = 0;
    if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1);
    fprintf(stderr, "easm: uncaught trap %d\n", code);
    abort();
}

// ---------------------------------------------------------------- const expr eval
int ea_eval_const_expr(EaInstance *inst, EaModule *m, const InsList *code, WVal *out,
                       EaValType expect) {
    WVal stack[16];
    (void)0;
    uint32_t sp = 0;
    for (uint32_t pc = 0; pc < code->n; pc++) {
        EaInstr *in = &code->v[pc];
        switch (in->opcode) {
        case EA_OP_I32_CONST: stack[sp++].i32 = in->imm.u32; break;
        case EA_OP_I64_CONST: stack[sp++].i64 = in->imm.u64; break;
        case EA_OP_F32_CONST: stack[sp++].f32 = in->imm.f32; break;
        case EA_OP_F64_CONST: stack[sp++].f64 = in->imm.f64; break;
        case EA_OP_V128_CONST:
            memcpy(stack[sp++].v128, in->imm.bytes, 16);
            break;
        case EA_OP_GLOBAL_GET: {
            uint32_t gi = in->imm.u32;
            if (!inst || gi >= inst->n_globals) return -1;
            stack[sp++] = inst->globals[gi];
            break;
        }
        case EA_OP_REF_NULL:
            stack[sp++].ref = NULL;
            break;
        case EA_OP_REF_FUNC: {
            if (!inst || in->imm.u32 >= inst->n_funcs) return -1;
            stack[sp++].ref = &inst->funcs[in->imm.u32];
            break;
        }
        case EA_OP_I32_ADD: sp--; stack[sp - 1].i32 += stack[sp].i32; break;
        case EA_OP_I32_SUB: sp--; stack[sp - 1].i32 -= stack[sp].i32; break;
        case EA_OP_I32_MUL: sp--; stack[sp - 1].i32 *= stack[sp].i32; break;
        case EA_OP_I64_ADD: sp--; stack[sp - 1].i64 += stack[sp].i64; break;
        case EA_OP_I64_SUB: sp--; stack[sp - 1].i64 -= stack[sp].i64; break;
        case EA_OP_I64_MUL: sp--; stack[sp - 1].i64 *= stack[sp].i64; break;
        default:
            return -1;
        }
    }
    if (sp == 0) return -1;
    *out = stack[--sp];
    (void)expect;
    (void)m;
    return 0;
}

// ---------------------------------------------------------------- memory alloc
#define EA_MEM_RESERVE ((size_t)(12ull << 30)) // 12 GiB VA per memory

uint8_t *alloc_memory(uint64_t pages);
uint8_t *alloc_memory_public(uint64_t pages) {
    return alloc_memory(pages);
}
uint8_t *alloc_memory(uint64_t pages) {
    void *p = mmap(NULL, EA_MEM_RESERVE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (pages > 0) {
        if (mprotect(p, (size_t)(pages << 16), PROT_READ | PROT_WRITE) != 0) {
            munmap(p, EA_MEM_RESERVE);
            return NULL;
        }
    }
    return (uint8_t *)p;
}
bool ea_grow_memory(EaMemInst *mi, uint64_t delta, uint64_t *old) {
    *old = mi->pages;
    if (mi->pages + delta > mi->max_pages) return false;
    if (mi->pages + delta > 65536) return false;
    if (mprotect(mi->base, (size_t)((mi->pages + delta) << 16),
                 PROT_READ | PROT_WRITE) != 0)
        return false;
    mi->pages += delta;
    mi->size = mi->pages << 16;
    return true;
}

void ea_instance_free(EaInstance *inst) {
    if (!inst) return;
    for (uint32_t i = 0; i < inst->n_tables; i++) free(inst->tables[i].elems);
    for (uint32_t i = 0; i < inst->n_memories; i++)
        if (inst->memories[i].owned && inst->memories[i].base)
            munmap(inst->memories[i].base, EA_MEM_RESERVE);
    free(inst->funcs);
    free(inst->tables);
    free(inst->memories);
    free(inst->globals);
    free(inst->tags);
    free(inst->data_alive);
    free(inst->elem_alive);
    free(inst);
}

// ---------------------------------------------------------------- instantiation
static bool type_matches(EaFuncType *a, EaFuncType *b) {
    if (a->n_params != b->n_params || a->n_results != b->n_results) return false;
    for (uint32_t i = 0; i < a->n_params; i++)
        if (a->params[i] != b->params[i]) return false;
    for (uint32_t i = 0; i < a->n_results; i++)
        if (a->results[i] != b->results[i]) return false;
    return true;
}

int ea_store_instantiate(EaStore *s, EaModule *m, EaInstance **out, char **err_msg, EaTrap *trap) {
    EaInstance *inst = (EaInstance *)ea_zalloc(sizeof(EaInstance));
    inst->module = m;
    inst->store = s;
    inst->start_func = m->has_start ? m->start_func : UINT32_MAX;
    const char *fail = NULL;

    uint32_t n_funcs = m->n_funcs, n_tables = m->n_tables, n_mems = m->n_memories,
             n_globals = m->n_globals_def, n_tags = m->n_tags;
    inst->n_funcs = n_funcs;
    inst->n_tables = n_tables;
    inst->n_memories = n_mems;
    inst->n_globals = n_globals;
    inst->n_tags = n_tags;
    inst->funcs = (EaFuncInst *)ea_zalloc((n_funcs ? n_funcs : 1) * sizeof(EaFuncInst));
    inst->tables = (EaTableInst *)ea_zalloc((n_tables ? n_tables : 1) * sizeof(EaTableInst));
    inst->memories = (EaMemInst *)ea_zalloc((n_mems ? n_mems : 1) * sizeof(EaMemInst));
    inst->globals = (WVal *)ea_zalloc((n_globals ? n_globals : 1) * sizeof(WVal));
    inst->tags = (EaTagInst *)ea_zalloc((n_tags ? n_tags : 1) * sizeof(EaTagInst));

    uint32_t ifunc = 0, itable = 0, imem = 0, iglob = 0;
    for (uint32_t i = 0; i < m->n_imports; i++) {
        EaImport *im = &m->imports[i];
        EaInstance *src = ea_lookup_instance(s, im->module);
        if (!src) {
            fail = "unknown import";
            goto unlinkable;
        }
        if (getenv("EA_IDBG")) fprintf(stderr, "IMP[%u] kind=%u %s.%s\n", i, im->kind, im->module, im->name);
        switch (im->kind) {
        case EAK_FUNC: {
            int idx = ea_instance_export(src, im->name, EAK_FUNC);
            if (idx < 0) { fail = "unknown import"; goto unlinkable; }
            EaFuncInst *fi = &src->funcs[idx];
            if (!type_matches(fi->type, &m->types[im->idx].func)) {
                if (getenv("EA_IDBG")) fprintf(stderr, "FAIL func import %s.%s\n", im->module, im->name);
                fail = "incompatible import type";
                goto unlinkable;
            }
            inst->funcs[ifunc++] = *fi;
            break;
        }
        case EAK_TABLE: {
            int idx = ea_instance_export(src, im->name, EAK_TABLE);
            if (idx < 0) { fail = "unknown import"; goto unlinkable; }
            EaTableInst *ti = &src->tables[idx];
            if (ti->ref_type != im->table.ref_type) {
                if (getenv("EA_IDBG")) fprintf(stderr, "FAIL table import reftype\n");
                fail = "incompatible import type"; goto unlinkable;
            }
            if (ti->size < im->table.min) { fail = "incompatible import type"; goto unlinkable; }
            if (im->table.has_max) {
                if (!ti->has_max || ti->max > im->table.max) {
                    fail = "incompatible import type";
                    goto unlinkable;
                }
            }
            inst->tables[itable++] = *ti;
            break;
        }
        case EAK_MEMORY: {
            int idx = ea_instance_export(src, im->name, EAK_MEMORY);
            if (idx < 0) { fail = "unknown import"; goto unlinkable; }
            EaMemInst *mi = &src->memories[idx];
            if (mi->pages < im->memory.min) { fail = "incompatible import type"; goto unlinkable; }
            if (im->memory.has_max) {
                uint64_t src_max = mi->has_max ? mi->max_pages : 65536;
                if (!mi->has_max && im->memory.max < 65536) {
                    fail = "incompatible import type";
                    goto unlinkable;
                }
                if (mi->has_max && src_max > im->memory.max) {
                    fail = "incompatible import type";
                    goto unlinkable;
                }
            }
            inst->memories[imem++] = *mi;
            break;
        }
        case EAK_GLOBAL: {
            int idx = ea_instance_export(src, im->name, EAK_GLOBAL);
            if (idx < 0) { fail = "unknown import"; goto unlinkable; }
            if (src->module->globals_def[idx].type != im->global.type ||
                src->module->globals_def[idx].mutable_ != im->global.mutable_) {
                if (getenv("EA_IDBG")) fprintf(stderr, "FAIL global: src.type=%d want=%d src.mut=%d want.mut=%d idx=%d n=%u\n",
                                               src->module->globals_def[idx].type, im->global.type,
                                               src->module->globals_def[idx].mutable_, im->global.mutable_,
                                               idx, src->module->n_globals_def);
                fail = "incompatible import type";
                goto unlinkable;
            }
            inst->globals[iglob++] = src->globals[idx];
            if (getenv("EA_IDBG")) fprintf(stderr, "IMP global %s.%s ok type=%d mut=%d\n",
                                           im->module, im->name, im->global.type, im->global.mutable_);
            break;
        }
        }
    }

    // ---- defined functions
    for (uint32_t i = m->n_imp_funcs; i < n_funcs; i++) {
        EaFuncInst *fi = &inst->funcs[i];
        fi->type = &m->types[m->funcs[i].type_idx].func;
        fi->inst = inst;
        fi->func_idx = i;
        fi->code = &m->funcs[i];
        fi->is_jit = m->funcs[i].jit_code != NULL;
        fi->jit_entry = m->funcs[i].jit_code;
    }

    // ---- globals (imports first, then defined, in order)
    for (uint32_t i = m->n_imp_globals; i < n_globals; i++) {
        EaGlobal *g = &m->globals_def[i];
        WVal v;
        if (ea_eval_const_expr(inst, m, &g->init, &v, g->type) != 0) {
            fail = "invalid global initializer";
            goto uninstantiable;
        }
        inst->globals[i] = v;
    }

    // ---- tables
    for (uint32_t i = m->n_imp_tables; i < n_tables; i++) {
        EaTable *t = &m->tables[i];
        EaTableInst *ti = &inst->tables[i];
        ti->ref_type = t->ref_type;
        ti->max = t->has_max ? t->max : UINT32_MAX;
        ti->has_max = t->has_max;
        ti->elems = (WVal *)ea_zalloc((t->min ? t->min : 1) * sizeof(WVal));
        ti->size = t->min;
        if (t->has_init) {
            WVal iv;
            if (ea_eval_const_expr(inst, m, &t->init, &iv, t->ref_type) != 0) {
                fail = "invalid table initializer";
                goto uninstantiable;
            }
            for (uint32_t k = 0; k < ti->size; k++) ti->elems[k] = iv;
        }
    }
    // ---- memories
    for (uint32_t i = m->n_imp_memories; i < n_mems; i++) {
        EaMemory *mm = &m->memories[i];
        EaMemInst *mi = &inst->memories[i];
        mi->base = alloc_memory(mm->min);
        if (!mi->base) { fail = "out of memory"; goto uninstantiable; }
        mi->pages = mm->min;
        mi->max_pages = mm->has_max ? mm->max : 65536;
        mi->has_max = mm->has_max;
        mi->size = mm->min << 16;
        mi->is64 = mm->is64;
        mi->owned = true;
    }

    inst->elem_alive = (uint8_t *)ea_zalloc(m->n_elems ? m->n_elems : 1);
    inst->data_alive = (uint8_t *)ea_zalloc(m->n_datas ? m->n_datas : 1);

    // ---- element segments
    for (uint32_t i = 0; i < m->n_elems; i++) {
        EaElem *e = &m->elems[i];
        if (e->mode == SEG_DECLARATIVE) continue;
        if (e->mode == SEG_PASSIVE) {
            inst->elem_alive[i] = 1;
            continue;
        }
        uint32_t n = e->n_items;
        WVal *vals = NULL;
        if (n) vals = (WVal *)ea_malloc(n * sizeof(WVal));
        for (uint32_t j = 0; j < n; j++) {
            if (e->items) {
                if (ea_eval_const_expr(inst, m, &e->items[j], &vals[j], e->ref_type) != 0) {
                    free(vals);
                    fail = "invalid element expression";
                    goto uninstantiable;
                }
            } else {
                vals[j].ref = &inst->funcs[e->func_idx[j]];
            }
        }
        WVal offv;
        if (ea_eval_const_expr(inst, m, &e->offset, &offv, VT_I32) != 0) {
            free(vals);
            fail = "invalid element offset";
            goto uninstantiable;
        }
        uint64_t off = (uint32_t)offv.i32;
        EaTableInst *ti = &inst->tables[e->table_idx];
        if (n > ti->size || off > ti->size - n) {
            free(vals);
            fail = "out of bounds table access";
            goto uninstantiable;
        }
        for (uint32_t j = 0; j < n; j++) ti->elems[off + j] = vals[j];
        free(vals);
    }

    // ---- data segments
    for (uint32_t i = 0; i < m->n_datas; i++) {
        EaData *d = &m->datas[i];
        if (d->mode != SEG_ACTIVE) {
            inst->data_alive[i] = 1;
            continue;
        }
        WVal offv;
        if (ea_eval_const_expr(inst, m, &d->offset, &offv, VT_I32) != 0) {
            fail = "invalid data offset";
            goto uninstantiable;
        }
        uint64_t off = (uint32_t)offv.i32;
        EaMemInst *mi = &inst->memories[d->mem_idx];
        if (d->data_len > mi->size || off > mi->size - d->data_len) {
            fail = "out of bounds memory access";
            goto uninstantiable;
        }
        if (d->data_len) memcpy(mi->base + off, m->owned_bytes + d->data_off, d->data_len);
    }

    // JIT fast-path fields
    if (n_mems) {
        inst->jit_mem_base = inst->memories[0].base;
        inst->jit_mem_limit = inst->memories[0].size;
    }
    inst->jit_globals = inst->globals;
    *out = inst;
    if (inst->start_func != UINT32_MAX) {
        EaTrap t = TRAP_NONE;
        int r = ea_instance_invoke(s, inst, inst->start_func, NULL, NULL, &t);
        if (r != 0) {
            if (trap) *trap = t;
            if (err_msg && !*err_msg)
                *err_msg = ea_strndup("start function trapped", 22);
            return 2;
        }
    }
    return 0;

unlinkable:
    if (err_msg && !*err_msg) *err_msg = ea_strndup(fail, strlen(fail));
    if (trap) *trap = TRAP_NONE;
    return 1;
uninstantiable:
    if (err_msg && !*err_msg) *err_msg = ea_strndup(fail, strlen(fail));
    if (trap) *trap = TRAP_OOB_MEMORY;
    return 2;
}

// ---------------------------------------------------------------- invoke
int ea_instance_invoke(EaStore *s, EaInstance *inst, uint32_t func_idx,
                       const WVal *args, WVal *results, EaTrap *trap) {
    EaExec *ex = &s->exec;
    if (func_idx >= inst->n_funcs) return -1;
    EaFuncInst *fi = &inst->funcs[func_idx];
    ex->trap = TRAP_NONE;
    jmp_buf jb;
    jmp_buf *saved = (jmp_buf *)ex->jb;
    ex->jb = &jb;
    int ret;
    if (setjmp(jb) == 0) {
        ex->depth = 0; // reset in case a previous call trapped mid-recursion
        uint32_t n_params = fi->type->n_params;
        if (interp_ensure(ex, n_params + 64) != 0) {
            ex->jb = saved;
            if (trap) *trap = TRAP_STACK_EXHAUSTED;
            return 2;
        }
        for (uint32_t i = 0; i < n_params; i++) ex->stack[ex->sp++] = args[i];
        int rc;
        if (fi->is_jit) {
            rc = ea_jit_call(ex, fi);
        } else if (fi->is_host) {
            WVal local_args[16];
            WVal local_results[16];
            for (uint32_t i = 0; i < n_params && i < 16; i++) local_args[i] = ex->stack[ex->sp - n_params + i];
            ex->sp -= n_params;
            memset(local_results, 0, sizeof(local_results));
            int hr = fi->host_fn(fi->host_user, local_args, local_results);
            rc = hr == 0 ? 0 : 1;
            if (rc == 0) {
                uint32_t n_results = fi->type->n_results;
                if (interp_ensure(ex, n_results) == 0) {
                    for (uint32_t i = 0; i < n_results; i++) ex->stack[ex->sp++] = local_results[i];
                } else {
                    rc = 1;
                    ex->trap = TRAP_STACK_EXHAUSTED;
                }
            } else {
                ex->trap = TRAP_HOST;
            }
        } else {
            rc = ea_interp_exec_function(ex, fi);
        }
        if (rc == 0 && results) {
            uint32_t n_results = fi->type->n_results;
            for (uint32_t i = 0; i < n_results; i++) results[i] = ex->stack[ex->sp - n_results + i];
            ex->sp -= n_results;
        }
        ex->jb = saved;
        ret = rc;
    } else {
        ex->jb = saved;
        ex->sp = 0;
        if (trap) *trap = ex->trap;
        ret = 1;
    }
    return ret;
}

int ea_store_load(EaStore *s, const uint8_t *bytes, size_t len, EaModule **out, char **err) {
    (void)s;
    EaModule *m = (EaModule *)ea_zalloc(sizeof(EaModule));
    int rc = ea_decode_module(m, bytes, len, err);
    if (rc != 0) {
        ea_module_free(m);
        free(m);
        return rc;
    }
    rc = ea_validate_module(m, err);
    if (rc != 0) {
        ea_module_free(m);
        free(m);
        return rc;
    }
    *out = m;
    return 0;
}
