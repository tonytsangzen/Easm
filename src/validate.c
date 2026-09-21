// easm validator: spec type-stack algorithm (appendix formulation)
#include "easm.h"
#include "opcodes.h"
#include <stdio.h>

#define MAX_LOCALS (1u << 24)

typedef struct {
    uint32_t height;          // value stack height at block entry (params excluded)
    const EaValType *in;      // label-in types (params)
    const EaValType *out;     // label-out types (results)
    uint32_t n_in, n_out;
    uint32_t start_idx;
    uint8_t is_loop;
    bool unreachable;
} Ctrl;

typedef struct {
    EaModule *m;
    EaValType *vals;
    uint32_t sp, cap;
    Ctrl *ctrl;
    uint32_t csp, ccap;
    uint32_t max_stack;
    char *err;
    bool failed;
} V;

// stable single-type arrays for blocktype kind==1
static EaValType g_single[8] = {VT_I32, VT_I64, VT_F32, VT_F64, VT_V128, VT_FUNCREF, VT_EXTERNREF, VT_ANYREF};

static void vfail(V *v, const char *msg) {
    if (!v->failed) {
        v->failed = true;
        if (v->err == NULL) v->err = ea_strndup(msg, strlen(msg));
    }
}
int g_vfail_pc = -1;
uint32_t g_vfail_sp, g_vfail_h;
int g_vfail_unre;

static bool push_val(V *v, EaValType t) {
    if (v->sp == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->vals = (EaValType *)ea_realloc(v->vals, v->cap * sizeof(EaValType));
    }
    v->vals[v->sp++] = t;
    if (v->sp > v->max_stack) v->max_stack = v->sp;
    return true;
}
static EaValType pop_val(V *v, EaValType expect) {
    Ctrl *c = &v->ctrl[v->csp - 1];
    if (v->sp == c->height) {
        if (c->unreachable) return expect == VT_BOTTOM ? VT_BOTTOM : expect;
        vfail(v, "type mismatch: stack underflow");
        return expect;
    }
    EaValType got = v->vals[--v->sp];
    if (got == VT_BOTTOM) return expect == VT_BOTTOM ? VT_BOTTOM : expect;
    if (expect != VT_BOTTOM && got != expect) {
        vfail(v, "type mismatch");
        return got;
    }
    return got;
}
static void push_vals(V *v, const EaValType *ts, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) push_val(v, ts[i]);
}
static void pop_vals(V *v, const EaValType *ts, uint32_t n) {
    for (uint32_t i = n; i > 0; i--) pop_val(v, ts[i - 1]);
}
static EaValType pop_any(V *v) {
    Ctrl *c = &v->ctrl[v->csp - 1];
    if (v->sp == c->height) {
        if (c->unreachable) return VT_BOTTOM;
        vfail(v, "type mismatch: stack underflow");
        return VT_BOTTOM;
    }
    return v->vals[--v->sp];
}
static void set_unreachable(V *v) {
    Ctrl *c = &v->ctrl[v->csp - 1];
    c->unreachable = true;
    v->sp = c->height;
}

static void push_ctrl(V *v, uint32_t start_idx, const EaValType *in, uint32_t n_in,
                      const EaValType *out, uint32_t n_out, bool is_loop) {
    if (v->csp == v->ccap) {
        v->ccap = v->ccap ? v->ccap * 2 : 16;
        v->ctrl = (Ctrl *)ea_realloc(v->ctrl, v->ccap * sizeof(Ctrl));
    }
    Ctrl *c = &v->ctrl[v->csp++];
    c->height = v->sp;
    c->in = in; c->n_in = n_in;
    c->out = out; c->n_out = n_out;
    c->start_idx = start_idx;
    c->is_loop = is_loop;
    c->unreachable = false;
    push_vals(v, in, n_in);
}
static bool pop_ctrl(V *v) {
    Ctrl *c = &v->ctrl[v->csp - 1];
    pop_vals(v, c->out, c->n_out);
    if (v->failed) return false;
    if (v->sp != c->height) {
        vfail(v, "type mismatch: values remaining at block end");
        return false;
    }
    v->csp--;
    return true;
}

static bool resolve_blocktype(V *v, const EaBlockType *bt,
                              const EaValType **in, uint32_t *n_in,
                              const EaValType **out, uint32_t *n_out) {
    if (bt->kind == 0) {
        *in = NULL; *n_in = 0;
        *out = NULL; *n_out = 0;
        return true;
    }
    if (bt->kind == 1) {
        *in = NULL; *n_in = 0;
        *out = &g_single[bt->vt & 7]; *n_out = 1;
        return true;
    }
    if (bt->type_idx >= v->m->n_types || v->m->types[bt->type_idx].kind != CT_FUNC) {
        vfail(v, "unknown type");
        return false;
    }
    const EaFuncType *ft = &v->m->types[bt->type_idx].func;
    *in = ft->params; *n_in = ft->n_params;
    *out = ft->results; *n_out = ft->n_results;
    return true;
}

// ---------------------------------------------------------------- signature table
typedef struct { uint8_t n_in, n_out; EaValType in[3], out[1]; } Sig;

bool simd_sig(uint32_t op, Sig *s);

static bool op_sig(uint32_t op, Sig *s) {
    s->n_in = s->n_out = 0;
    switch (op) {
    case EA_OP_NOP: case EA_OP_UNREACHABLE: case EA_OP_DROP: case EA_OP_ELSE: case EA_OP_END:
        return true;
    case EA_OP_I32_EQZ:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I64_EQZ:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I32_EQ: case EA_OP_I32_NE: case EA_OP_I32_LT_S: case EA_OP_I32_LT_U:
    case EA_OP_I32_GT_S: case EA_OP_I32_GT_U: case EA_OP_I32_LE_S: case EA_OP_I32_LE_U:
    case EA_OP_I32_GE_S: case EA_OP_I32_GE_U:
        s->n_in = 2; s->in[0] = s->in[1] = VT_I32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I64_EQ: case EA_OP_I64_NE: case EA_OP_I64_LT_S: case EA_OP_I64_LT_U:
    case EA_OP_I64_GT_S: case EA_OP_I64_GT_U: case EA_OP_I64_LE_S: case EA_OP_I64_LE_U:
    case EA_OP_I64_GE_S: case EA_OP_I64_GE_U:
        s->n_in = 2; s->in[0] = s->in[1] = VT_I64; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_F32_EQ: case EA_OP_F32_NE: case EA_OP_F32_LT: case EA_OP_F32_GT:
    case EA_OP_F32_LE: case EA_OP_F32_GE:
        s->n_in = 2; s->in[0] = s->in[1] = VT_F32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_F64_EQ: case EA_OP_F64_NE: case EA_OP_F64_LT: case EA_OP_F64_GT:
    case EA_OP_F64_LE: case EA_OP_F64_GE:
        s->n_in = 2; s->in[0] = s->in[1] = VT_F64; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I32_CLZ: case EA_OP_I32_CTZ: case EA_OP_I32_POPCNT:
    case EA_OP_I32_EXTEND8_S: case EA_OP_I32_EXTEND16_S:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I32_ADD: case EA_OP_I32_SUB: case EA_OP_I32_MUL:
    case EA_OP_I32_DIV_S: case EA_OP_I32_DIV_U: case EA_OP_I32_REM_S: case EA_OP_I32_REM_U:
    case EA_OP_I32_AND: case EA_OP_I32_OR: case EA_OP_I32_XOR:
    case EA_OP_I32_SHL: case EA_OP_I32_SHR_S: case EA_OP_I32_SHR_U:
    case EA_OP_I32_ROTL: case EA_OP_I32_ROTR:
        s->n_in = 2; s->in[0] = s->in[1] = VT_I32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I64_CLZ: case EA_OP_I64_CTZ: case EA_OP_I64_POPCNT:
    case EA_OP_I64_EXTEND8_S: case EA_OP_I64_EXTEND16_S: case EA_OP_I64_EXTEND32_S:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_I64; return true;
    case EA_OP_I64_ADD: case EA_OP_I64_SUB: case EA_OP_I64_MUL:
    case EA_OP_I64_DIV_S: case EA_OP_I64_DIV_U: case EA_OP_I64_REM_S: case EA_OP_I64_REM_U:
    case EA_OP_I64_AND: case EA_OP_I64_OR: case EA_OP_I64_XOR:
    case EA_OP_I64_SHL: case EA_OP_I64_SHR_S: case EA_OP_I64_SHR_U:
    case EA_OP_I64_ROTL: case EA_OP_I64_ROTR:
        s->n_in = 2; s->in[0] = s->in[1] = VT_I64; s->n_out = 1; s->out[0] = VT_I64; return true;
    case EA_OP_F32_ABS: case EA_OP_F32_NEG: case EA_OP_F32_CEIL: case EA_OP_F32_FLOOR:
    case EA_OP_F32_TRUNC: case EA_OP_F32_NEAREST: case EA_OP_F32_SQRT:
        s->n_in = 1; s->in[0] = VT_F32; s->n_out = 1; s->out[0] = VT_F32; return true;
    case EA_OP_F32_ADD: case EA_OP_F32_SUB: case EA_OP_F32_MUL: case EA_OP_F32_DIV:
    case EA_OP_F32_MIN: case EA_OP_F32_MAX: case EA_OP_F32_COPYSIGN:
        s->n_in = 2; s->in[0] = s->in[1] = VT_F32; s->n_out = 1; s->out[0] = VT_F32; return true;
    case EA_OP_F64_ABS: case EA_OP_F64_NEG: case EA_OP_F64_CEIL: case EA_OP_F64_FLOOR:
    case EA_OP_F64_TRUNC: case EA_OP_F64_NEAREST: case EA_OP_F64_SQRT:
        s->n_in = 1; s->in[0] = VT_F64; s->n_out = 1; s->out[0] = VT_F64; return true;
    case EA_OP_F64_ADD: case EA_OP_F64_SUB: case EA_OP_F64_MUL: case EA_OP_F64_DIV:
    case EA_OP_F64_MIN: case EA_OP_F64_MAX: case EA_OP_F64_COPYSIGN:
        s->n_in = 2; s->in[0] = s->in[1] = VT_F64; s->n_out = 1; s->out[0] = VT_F64; return true;
    case EA_OP_I32_WRAP_I64:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I32_TRUNC_F32_S: case EA_OP_I32_TRUNC_F32_U:
    case EA_OP_I32_TRUNC_SAT_F32_S: case EA_OP_I32_TRUNC_SAT_F32_U:
    case EA_OP_I32_REINTERPRET_F32:
        s->n_in = 1; s->in[0] = VT_F32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I32_TRUNC_F64_S: case EA_OP_I32_TRUNC_F64_U:
    case EA_OP_I32_TRUNC_SAT_F64_S: case EA_OP_I32_TRUNC_SAT_F64_U:
        s->n_in = 1; s->in[0] = VT_F64; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I64_EXTEND_I32_S: case EA_OP_I64_EXTEND_I32_U:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_I64; return true;
    case EA_OP_I64_TRUNC_F32_S: case EA_OP_I64_TRUNC_F32_U:
    case EA_OP_I64_TRUNC_SAT_F32_S: case EA_OP_I64_TRUNC_SAT_F32_U:
        s->n_in = 1; s->in[0] = VT_F32; s->n_out = 1; s->out[0] = VT_I64; return true;
    case EA_OP_I64_TRUNC_F64_S: case EA_OP_I64_TRUNC_F64_U:
    case EA_OP_I64_TRUNC_SAT_F64_S: case EA_OP_I64_TRUNC_SAT_F64_U:
    case EA_OP_I64_REINTERPRET_F64:
        s->n_in = 1; s->in[0] = VT_F64; s->n_out = 1; s->out[0] = VT_I64; return true;
    case EA_OP_F32_CONVERT_I32_S: case EA_OP_F32_CONVERT_I32_U:
    case EA_OP_F32_REINTERPRET_I32:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_F32; return true;
    case EA_OP_F32_CONVERT_I64_S: case EA_OP_F32_CONVERT_I64_U:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_F32; return true;
    case EA_OP_F32_DEMOTE_F64:
        s->n_in = 1; s->in[0] = VT_F64; s->n_out = 1; s->out[0] = VT_F32; return true;
    case EA_OP_F64_CONVERT_I32_S: case EA_OP_F64_CONVERT_I32_U:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_F64; return true;
    case EA_OP_F64_CONVERT_I64_S: case EA_OP_F64_CONVERT_I64_U:
    case EA_OP_F64_REINTERPRET_I64:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_F64; return true;
    case EA_OP_F64_PROMOTE_F32:
        s->n_in = 1; s->in[0] = VT_F32; s->n_out = 1; s->out[0] = VT_F64; return true;
    case EA_OP_REF_IS_NULL:
        s->n_in = 1; s->in[0] = VT_BOTTOM; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_REF_EQ:
        s->n_in = 2; s->in[0] = VT_BOTTOM; s->in[1] = VT_BOTTOM;
        s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_MEMORY_SIZE:
        s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_MEMORY_GROW:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_TABLE_SIZE:
        s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_TABLE_GROW:
        s->n_in = 2; s->in[0] = VT_BOTTOM; s->in[1] = VT_I32;
        s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_TABLE_FILL:
        s->n_in = 3; s->in[0] = VT_I32; s->in[1] = VT_BOTTOM; s->in[2] = VT_I32; return true;
    case EA_OP_TABLE_COPY: case EA_OP_TABLE_INIT:
    case EA_OP_MEMORY_FILL: case EA_OP_MEMORY_COPY: case EA_OP_MEMORY_INIT:
        s->n_in = 3; s->in[0] = s->in[1] = s->in[2] = VT_I32; return true;
    case EA_OP_DATA_DROP: case EA_OP_ELEM_DROP:
        return true;
    default:
        if ((op & 0xFF00) == EA_OPV_BASE) return simd_sig(op, s);
        return false;
    }
}

static bool is_load(uint32_t op) {
    return (op >= EA_OP_I32_LOAD && op <= EA_OP_I64_LOAD32_U);
}
static bool is_store(uint32_t op) {
    return (op >= EA_OP_I32_STORE && op <= EA_OP_I64_STORE32);
}
static uint8_t load_width(uint32_t op) {
    switch (op) {
    case EA_OP_I32_LOAD8_S: case EA_OP_I32_LOAD8_U:
    case EA_OP_I64_LOAD8_S: case EA_OP_I64_LOAD8_U: return 1;
    case EA_OP_I32_LOAD16_S: case EA_OP_I32_LOAD16_U:
    case EA_OP_I64_LOAD16_S: case EA_OP_I64_LOAD16_U: return 2;
    case EA_OP_I32_LOAD: case EA_OP_F32_LOAD:
    case EA_OP_I64_LOAD32_S: case EA_OP_I64_LOAD32_U: return 4;
    default: return 8;
    }
}
static uint8_t store_width(uint32_t op) {
    switch (op) {
    case EA_OP_I32_STORE8: case EA_OP_I64_STORE8: return 1;
    case EA_OP_I32_STORE16: case EA_OP_I64_STORE16: return 2;
    case EA_OP_I32_STORE: case EA_OP_F32_STORE:
    case EA_OP_I64_STORE32: return 4;
    default: return 8;
    }
}

// type-check a const expr; returns resulting valtype or VT_BOTTOM on error
static EaValType check_const_expr(EaModule *m, const InsList *code, uint32_t n_globals_visible,
                                  char **err) {
    EaValType stack[8];
    uint32_t sp = 0;
    if (code->n == 0) goto bad;
    for (uint32_t k = 0; k < code->n; k++) {
        EaInstr *in = &code->v[k];
        switch (in->opcode) {
        case EA_OP_I32_CONST: case EA_OP_REF_NULL:
            if (sp >= 8) goto bad;
            stack[sp++] = (in->opcode == EA_OP_I32_CONST) ? VT_I32 : (EaValType)in->imm.u32;
            break;
        case EA_OP_I64_CONST: if (sp >= 8) goto bad; stack[sp++] = VT_I64; break;
        case EA_OP_F32_CONST: if (sp >= 8) goto bad; stack[sp++] = VT_F32; break;
        case EA_OP_F64_CONST: if (sp >= 8) goto bad; stack[sp++] = VT_F64; break;
        case EA_OP_V128_CONST: if (sp >= 8) goto bad; stack[sp++] = VT_V128; break;
        case EA_OP_REF_FUNC:
            if (in->imm.u32 >= m->n_funcs) { if (err && !*err) *err = ea_strndup("unknown function", 16); goto bad; }
            if (sp >= 8) goto bad;
            stack[sp++] = VT_FUNCREF;
            break;
        case EA_OP_GLOBAL_GET: {
            uint32_t gi = in->imm.u32;
            if (gi >= n_globals_visible) { if (err && !*err) *err = ea_strndup("unknown global", 14); goto bad; }
            if (m->globals_def[gi].mutable_) { if (err && !*err) *err = ea_strndup("constant expression required", 28); goto bad; }
            if (sp >= 8) goto bad;
            stack[sp++] = m->globals_def[gi].type;
            break;
        }
        // extended constant expressions (Wasm 3.0)
        case EA_OP_I32_ADD: case EA_OP_I32_SUB: case EA_OP_I32_MUL:
        case EA_OP_I64_ADD: case EA_OP_I64_SUB: case EA_OP_I64_MUL: {
            if (sp < 2) goto bad;
            EaValType a = stack[sp - 2], b = stack[sp - 1];
            EaValType want = (in->opcode == EA_OP_I32_ADD || in->opcode == EA_OP_I32_SUB ||
                              in->opcode == EA_OP_I32_MUL) ? VT_I32 : VT_I64;
            if (a != want || b != want) goto bad;
            sp--;
            break;
        }
        default:
            if (err && !*err) *err = ea_strndup("constant expression required", 28);
            goto bad;
        }
    }
    return stack[--sp];
bad:
    return VT_BOTTOM;
}

int ea_validate_module(EaModule *m, char **err) {
    int rc = 0;
    // v must be zeroed before any EXPR_FAIL goto out
    V v;
    memset(&v, 0, sizeof(v));
    // ---- module-level checks
    for (uint32_t i = 0; i < m->n_imports; i++) {
        EaImport *im = &m->imports[i];
        if (im->kind == EAK_FUNC &&
            (im->idx >= m->n_types || m->types[im->idx].kind != CT_FUNC)) {
            if (err && !*err) *err = ea_strndup("unknown import type", 18);
            return 1;
        }
    }
    for (uint32_t i = 0; i < m->n_funcs; i++) {
        if (m->funcs[i].type_idx >= m->n_types ||
            m->types[m->funcs[i].type_idx].kind != CT_FUNC) {
            if (err && !*err) *err = ea_strndup("unknown type", 12);
            return 1;
        }
    }
    for (uint32_t i = 0; i < m->n_tables; i++) {
        if (m->tables[i].has_max && m->tables[i].max < m->tables[i].min) {
            if (err && !*err) *err = ea_strndup("size minimum must not be greater than maximum", 43);
            return 1;
        }
    }
    if (m->n_memories == 1 && m->memories[0].shared) {
        if (err && !*err) *err = ea_strndup("shared memory requires threads", 29);
        return 1;
    }
    if (m->has_start) {
        if (m->start_func >= m->n_funcs) {
            if (err && !*err) *err = ea_strndup("unknown function (start)", 24);
            return 1;
        }
        EaFuncType *ft = &m->types[m->funcs[m->start_func].type_idx].func;
        if (ft->n_params != 0 || ft->n_results != 0) {
            if (err && !*err) *err = ea_strndup("start function must not have parameters or results", 49);
            return 1;
        }
    }
    for (uint32_t i = 0; i < m->n_exports; i++) {
        EaExport *ex = &m->exports[i];
        bool bad = false;
        switch (ex->kind) {
        case EAK_FUNC: bad = ex->idx >= m->n_funcs; break;
        case EAK_TABLE: bad = ex->idx >= m->n_tables; break;
        case EAK_MEMORY: bad = ex->idx >= m->n_memories; break;
        case EAK_GLOBAL: bad = ex->idx >= m->n_globals_def; break;
        case EAK_TAG: bad = true; break;
        }
        if (bad) {
            if (err && !*err) *err = ea_strndup("unknown entity in export", 24);
            return 1;
        }
    }

    // refs set (funcidx(module) outside function bodies)
    uint8_t *in_refs = (uint8_t *)ea_zalloc(m->n_funcs ? m->n_funcs : 1);
    for (uint32_t i = 0; i < m->n_exports; i++)
        if (m->exports[i].kind == EAK_FUNC && m->exports[i].idx < m->n_funcs)
            in_refs[m->exports[i].idx] = 1;
    if (m->has_start && m->start_func < m->n_funcs) in_refs[m->start_func] = 1;
    for (uint32_t i = 0; i < m->n_elems; i++) {
        EaElem *e = &m->elems[i];
        if (e->func_idx)
            for (uint32_t j = 0; j < e->n_items; j++)
                if (e->func_idx[j] < m->n_funcs) in_refs[e->func_idx[j]] = 1;
        if (e->items)
            for (uint32_t j = 0; j < e->n_items; j++)
                for (uint32_t k = 0; k < e->items[j].n; k++)
                    if (e->items[j].v[k].opcode == EA_OP_REF_FUNC &&
                        e->items[j].v[k].imm.u32 < m->n_funcs)
                        in_refs[e->items[j].v[k].imm.u32] = 1;
        for (uint32_t k = 0; k < e->offset.n; k++)
            if (e->offset.v[k].opcode == EA_OP_REF_FUNC && e->offset.v[k].imm.u32 < m->n_funcs)
                in_refs[e->offset.v[k].imm.u32] = 1;
    }
    for (uint32_t i = 0; i < m->n_globals_def; i++)
        for (uint32_t k = 0; k < m->globals_def[i].init.n; k++)
            if (m->globals_def[i].init.v[k].opcode == EA_OP_REF_FUNC &&
                m->globals_def[i].init.v[k].imm.u32 < m->n_funcs)
                in_refs[m->globals_def[i].init.v[k].imm.u32] = 1;

#define EXPR_FAIL(msg) do { if (err && !*err) *err = ea_strndup(msg, strlen(msg)); rc = 1; goto out; } while (0)



    // ---- init expr checks
    for (uint32_t i = m->n_imp_globals; i < m->n_globals_def; i++) {
        EaGlobal *g = &m->globals_def[i];
        EaValType rt = check_const_expr(m, &g->init, i + 1, NULL);
        if (getenv("EA_GDBG")) fprintf(stderr, "GLB %u: rt=%d want=%d n=%u\n", i, rt, g->type, g->init.n);
        if (rt != g->type) EXPR_FAIL("type mismatch: global init type");
    }
    for (uint32_t i = 0; i < m->n_elems; i++) {
        EaElem *e = &m->elems[i];
        if (e->mode == SEG_ACTIVE) {
            if (e->table_idx >= m->n_tables) EXPR_FAIL("unknown table");
            if (m->tables[e->table_idx].ref_type != e->ref_type)
                EXPR_FAIL("type mismatch: element segment type");
            EaValType rt = check_const_expr(m, &e->offset, m->n_globals_def, NULL);
            if (rt != VT_I32) EXPR_FAIL("type mismatch in element offset");
        }
        if (!e->items) {
            if (e->ref_type != VT_FUNCREF) EXPR_FAIL("type mismatch: element segment type");
            for (uint32_t j = 0; j < e->n_items; j++)
                if (e->func_idx[j] >= m->n_funcs) EXPR_FAIL("unknown function");
            continue;
        }
        for (uint32_t j = 0; j < e->n_items; j++) {
            EaValType rt = check_const_expr(m, &e->items[j], m->n_globals_def, NULL);
            if (!ea_is_ref(rt)) EXPR_FAIL("type mismatch: element item type");
        }
    }
    for (uint32_t i = 0; i < m->n_datas; i++) {
        EaData *d = &m->datas[i];
        if (d->mode == SEG_ACTIVE) {
            if (d->mem_idx >= m->n_memories) EXPR_FAIL("unknown memory");
            EaValType rt = check_const_expr(m, &d->offset, m->n_globals_def, NULL);
            EaValType want = m->memories[d->mem_idx].is64 ? VT_I64 : VT_I32;
            if (rt != want) EXPR_FAIL("type mismatch in data offset");
        }
    }

    // ---- function bodies
    v.m = m;
    for (uint32_t fi = 0; fi < m->n_funcs; fi++) {
        EaFunc *f = &m->funcs[fi];
        const EaFuncType *ft = &m->types[f->type_idx].func;
        // imported functions have no body to validate; still merge locals list
        if (fi < m->n_imp_funcs) {
            uint32_t n_locals = ft->n_params;
            EaValType *locals = (EaValType *)ea_malloc((n_locals ? n_locals : 1) * sizeof(EaValType));
            for (uint32_t i = 0; i < n_locals; i++) locals[i] = ft->params[i];
            free(f->locals);
            f->locals = locals;
            f->n_locals = n_locals;
            f->max_stack = 0;
            continue;
        }
        uint32_t n_locals = ft->n_params + f->n_locals;
        if (n_locals > MAX_LOCALS) EXPR_FAIL("too many locals");
        EaValType *locals = (EaValType *)ea_malloc((n_locals ? n_locals : 1) * sizeof(EaValType));
        for (uint32_t i = 0; i < ft->n_params; i++) locals[i] = ft->params[i];
        for (uint32_t i = 0; i < f->n_locals; i++)
            locals[ft->n_params + i] = f->locals[i];
        free(f->locals);
        f->locals = locals;
        f->n_locals = n_locals;

        v.sp = 0;
        v.csp = 0;
        v.failed = false;
        v.max_stack = 0;
        v.err = NULL;
        push_ctrl(&v, 0, NULL, 0, ft->results, ft->n_results, false);
        InsList *code = &f->code;
        uint32_t cur_pc = 0;
        uint16_t *depths = (uint16_t *)ea_zalloc((code->n ? code->n : 1) * 2);
        uint8_t *reach = (uint8_t *)ea_zalloc(code->n ? code->n : 1);
        uint16_t *br_ar = (uint16_t *)ea_zalloc((code->n ? code->n : 1) * 2);
        uint8_t cur_reach = 1;
        for (uint32_t pc = 0; pc < code->n && !v.failed; pc++) {
            cur_pc = pc;
            EaInstr *in = &code->v[pc];
            uint32_t op = in->opcode;
            depths[pc] = (uint16_t)v.sp;
            reach[pc] = cur_reach;
            if (v.csp == 0) { vfail(&v, "junk after end of function"); break; }
            Sig sig;
            switch (op) {
            case EA_OP_BLOCK: case EA_OP_LOOP: case EA_OP_IF: {
                const EaValType *bi, *bo;
                uint32_t nbi, nbo;
                if (!resolve_blocktype(&v, &in->imm.bt, &bi, &nbi, &bo, &nbo)) break;
                if ((nbi > 1 || nbo > 1) && !m->feat.multi_value) {
                    vfail(&v, "multi-value not enabled");
                    break;
                }
                if (op == EA_OP_IF) pop_val(&v, VT_I32);
                if (v.failed) break;
                pop_vals(&v, bi, nbi);
                if (v.failed) break;
                push_ctrl(&v, pc, bi, nbi, bo, nbo, op == EA_OP_LOOP);
                in->height = v.ctrl[v.csp - 1].height;
                in->is_loop = op == EA_OP_LOOP;
                in->arity_out = (uint16_t)(op == EA_OP_LOOP ? nbi : nbo);
                in->arity_in = (uint16_t)nbi;
                in->arity_res = (uint16_t)nbo;
                break;
            }
            case EA_OP_ELSE: {
                Ctrl *c = &v.ctrl[v.csp - 1];
                if (c->start_idx >= code->n ||
                    code->v[c->start_idx].opcode != EA_OP_IF) {
                    vfail(&v, "else found outside of if");
                    break;
                }
                if (!pop_ctrl(&v)) break;
                // re-push as else frame (same signature)
                EaInstr *ifin = &code->v[c->start_idx];
                const EaValType *bi, *bo;
                uint32_t nbi, nbo;
                if (!resolve_blocktype(&v, &ifin->imm.bt, &bi, &nbi, &bo, &nbo)) break;
                push_ctrl(&v, c->start_idx, bi, nbi, bo, nbo, false);
                break;
            }
            case EA_OP_END: {
                Ctrl *c = &v.ctrl[v.csp - 1];
                if (v.csp == 1) {
                    // function frame: results = function results
                    if (!pop_ctrl(&v)) break;
                    push_vals(&v, ft->results, ft->n_results);
                } else {
                    EaInstr *bin = &code->v[c->start_idx];
                    if (bin->opcode == EA_OP_IF && bin->else_idx == UINT32_MAX) {
                        // if without else: in must equal out
                        const EaValType *bi, *bo;
                        uint32_t nbi, nbo;
                        if (!resolve_blocktype(&v, &bin->imm.bt, &bi, &nbi, &bo, &nbo)) break;
                        if (nbi != nbo || memcmp(bi, bo, nbi * sizeof(EaValType)) != 0) {
                            vfail(&v, "type mismatch: if without else");
                            break;
                        }
                    }
                    if (!pop_ctrl(&v)) break;
                    push_vals(&v, c->out, c->n_out);
                }
                break;
            }
            case EA_OP_BR: {
                uint32_t l = in->imm.u32;
                if (l >= v.csp) { vfail(&v, "unknown label"); break; }
                Ctrl *t = &v.ctrl[v.csp - 1 - l];
                pop_vals(&v, t->is_loop ? t->in : t->out, t->is_loop ? t->n_in : t->n_out);
                if (!v.failed) {
                    // br target join point: record carried arity for codegen
                    if (pc + 1 < code->n + 1) cur_reach = 0;
                    set_unreachable(&v);
                }
                break;
            }
            case EA_OP_BR_IF: {
                pop_val(&v, VT_I32);
                if (v.failed) break;
                uint32_t l = in->imm.u32;
                if (l >= v.csp) { vfail(&v, "unknown label"); break; }
                Ctrl *t = &v.ctrl[v.csp - 1 - l];
                const EaValType *lt = t->is_loop ? t->in : t->out;
                uint32_t n = t->is_loop ? t->n_in : t->n_out;
                pop_vals(&v, lt, n);
                if (!v.failed) push_vals(&v, lt, n);
                break;
            }
            case EA_OP_BR_TABLE: {
                pop_val(&v, VT_I32);
                if (v.failed) break;
                uint32_t base = in->imm.pair.a, n = in->imm.pair.b;
                uint32_t *tg = &code->pool[base];
                uint32_t dl = tg[n];
                if (dl >= v.csp) { vfail(&v, "unknown label"); break; }
                Ctrl *dt = &v.ctrl[v.csp - 1 - dl];
                const EaValType *lt = dt->is_loop ? dt->in : dt->out;
                uint32_t na = dt->is_loop ? dt->n_in : dt->n_out;
                for (uint32_t i = 0; i < n && !v.failed; i++) {
                    uint32_t l = tg[i];
                    if (l >= v.csp) { vfail(&v, "unknown label"); break; }
                    Ctrl *t = &v.ctrl[v.csp - 1 - l];
                    const EaValType *lt2 = t->is_loop ? t->in : t->out;
                    uint32_t na2 = t->is_loop ? t->n_in : t->n_out;
                    if (na2 != na || memcmp(lt, lt2, na * sizeof(EaValType)) != 0)
                        vfail(&v, "type mismatch: br_table target types");
                }
                if (v.failed) break;
                pop_vals(&v, lt, na);
                if (!v.failed) set_unreachable(&v);
                break;
            }
            case EA_OP_RETURN: {
                pop_vals(&v, ft->results, ft->n_results);
                if (!v.failed) set_unreachable(&v);
                break;
            }
            case EA_OP_CALL: case EA_OP_RETURN_CALL: {
                uint32_t fi2 = in->imm.u32;
                if (fi2 >= m->n_funcs) { vfail(&v, "unknown function"); break; }
                const EaFuncType *t2 = &m->types[m->funcs[fi2].type_idx].func;
                if (op == EA_OP_RETURN_CALL) {
                    if (t2->n_results != ft->n_results ||
                        memcmp(t2->results, ft->results, t2->n_results * sizeof(EaValType)) != 0) {
                        vfail(&v, "type mismatch: return_call result types");
                        break;
                    }
                }
                pop_vals(&v, t2->params, t2->n_params);
                if (!v.failed) {
                    if (op == EA_OP_CALL) push_vals(&v, t2->results, t2->n_results);
                    else set_unreachable(&v);
                }
                break;
            }
            case EA_OP_CALL_INDIRECT: case EA_OP_RETURN_CALL_INDIRECT: {
                uint32_t ti = in->imm.pair.a, tbi = in->imm.pair.b;
                if (ti >= m->n_types || m->types[ti].kind != CT_FUNC) {
                    vfail(&v, "unknown type"); break;
                }
                if (tbi >= m->n_tables || m->tables[tbi].ref_type != VT_FUNCREF) {
                    vfail(&v, "unknown table"); break;
                }
                const EaFuncType *t2 = &m->types[ti].func;
                pop_val(&v, VT_I32);
                if (v.failed) break;
                if (op == EA_OP_RETURN_CALL_INDIRECT) {
                    if (t2->n_results != ft->n_results ||
                        memcmp(t2->results, ft->results, t2->n_results * sizeof(EaValType)) != 0) {
                        vfail(&v, "type mismatch: return_call_indirect result types");
                        break;
                    }
                }
                pop_vals(&v, t2->params, t2->n_params);
                if (!v.failed) {
                    if (op == EA_OP_CALL_INDIRECT) push_vals(&v, t2->results, t2->n_results);
                    else set_unreachable(&v);
                }
                break;
            }
            case EA_OP_DROP:
                pop_any(&v);
                break;
            case EA_OP_SELECT: {
                pop_val(&v, VT_I32);
                if (v.failed) break;
                EaValType t1 = pop_any(&v);
                EaValType t2 = pop_any(&v);
                if (v.failed) break;
                if (t1 == VT_BOTTOM) t1 = t2;
                if (t2 == VT_BOTTOM) t2 = t1;
                if (t1 != t2 || ea_is_ref(t1) || t1 == VT_V128) {
                    vfail(&v, "type mismatch: select operand types");
                    break;
                }
                push_val(&v, t1);
                break;
            }
            case EA_OP_SELECT_T: {
                pop_val(&v, VT_I32);
                if (v.failed) break;
                EaValType t = (EaValType)in->imm.u32;
                pop_val(&v, t);
                pop_val(&v, t);
                if (!v.failed) push_val(&v, t);
                break;
            }
            case EA_OP_LOCAL_GET:
                if (in->imm.u32 >= f->n_locals) { vfail(&v, "unknown local"); break; }
                push_val(&v, f->locals[in->imm.u32]);
                break;
            case EA_OP_LOCAL_SET:
                if (in->imm.u32 >= f->n_locals) { vfail(&v, "unknown local"); break; }
                pop_val(&v, f->locals[in->imm.u32]);
                break;
            case EA_OP_LOCAL_TEE:
                if (in->imm.u32 >= f->n_locals) { vfail(&v, "unknown local"); break; }
                pop_val(&v, f->locals[in->imm.u32]);
                if (!v.failed) push_val(&v, f->locals[in->imm.u32]);
                break;
            case EA_OP_GLOBAL_GET:
                if (in->imm.u32 >= m->n_globals_def) { vfail(&v, "unknown global"); break; }
                push_val(&v, m->globals_def[in->imm.u32].type);
                break;
            case EA_OP_GLOBAL_SET:
                if (in->imm.u32 >= m->n_globals_def) { vfail(&v, "unknown global"); break; }
                if (!m->globals_def[in->imm.u32].mutable_) { vfail(&v, "global is immutable"); break; }
                pop_val(&v, m->globals_def[in->imm.u32].type);
                break;
            case EA_OP_TABLE_GET: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                pop_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                if (!v.failed) push_val(&v, m->tables[ti].ref_type);
                break;
            }
            case EA_OP_TABLE_SET: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                pop_val(&v, m->tables[ti].ref_type);
                if (!v.failed) pop_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                break;
            }
            case EA_OP_TABLE_SIZE: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                push_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                break;
            }
            case EA_OP_TABLE_GROW: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                pop_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                if (!v.failed) pop_val(&v, m->tables[ti].ref_type);
                if (!v.failed) push_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                break;
            }
            case EA_OP_TABLE_FILL: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                pop_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                if (!v.failed) pop_val(&v, m->tables[ti].ref_type);
                if (!v.failed) pop_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                break;
            }
            case EA_OP_TABLE_COPY: {
                uint32_t d = in->imm.pair.a, s2 = in->imm.pair.b;
                if (d >= m->n_tables || s2 >= m->n_tables) { vfail(&v, "unknown table"); break; }
                if (m->tables[d].ref_type != m->tables[s2].ref_type) {
                    vfail(&v, "type mismatch: table copy element types");
                    break;
                }
                EaValType it = m->tables[d].is64 ? VT_I64 : VT_I32;
                pop_val(&v, it); pop_val(&v, it); pop_val(&v, it);
                break;
            }
            case EA_OP_TABLE_INIT: {
                uint32_t ti = in->imm.pair.a, ei = in->imm.pair.b;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                if (ei >= m->n_elems) { vfail(&v, "unknown elem segment"); break; }
                if (m->elems[ei].mode == SEG_ACTIVE) { vfail(&v, "element segment not passive"); break; }
                if (m->elems[ei].ref_type != m->tables[ti].ref_type) {
                    vfail(&v, "type mismatch: table init element types");
                    break;
                }
                EaValType it = m->tables[ti].is64 ? VT_I64 : VT_I32;
                pop_val(&v, it); pop_val(&v, it); pop_val(&v, it);
                break;
            }
            case EA_OP_ELEM_DROP: {
                uint32_t ei = in->imm.u32;
                if (ei >= m->n_elems) { vfail(&v, "unknown elem segment"); break; }
                if (m->elems[ei].mode == SEG_ACTIVE) { vfail(&v, "element segment not passive"); break; }
                break;
            }
            case EA_OP_REF_NULL:
                push_val(&v, (EaValType)in->imm.u32);
                break;
            case EA_OP_REF_FUNC:
                if (in->imm.u32 >= m->n_funcs) { vfail(&v, "unknown function"); break; }
                if (!in_refs[in->imm.u32]) { vfail(&v, "undeclared function reference"); break; }
                push_val(&v, VT_FUNCREF);
                break;
            case EA_OP_BR_ON_NULL: case EA_OP_BR_ON_NON_NULL:
            case EA_OP_REF_AS_NON_NULL:
            case EA_OP_CALL_REF: case EA_OP_RETURN_CALL_REF:
                vfail(&v, "typed function references are not enabled yet");
                break;
            case EA_OP_UNREACHABLE:
                set_unreachable(&v);
                break;
            case EA_OP_NOP:
                break;
            case EA_OP_I32_CONST: push_val(&v, VT_I32); break;
            case EA_OP_I64_CONST: push_val(&v, VT_I64); break;
            case EA_OP_F32_CONST: push_val(&v, VT_F32); break;
            case EA_OP_F64_CONST: push_val(&v, VT_F64); break;
            default: {
                if (is_load(op)) {
                    uint32_t align = in->imm.pair.a;
                    uint32_t memidx = in->imm.q.c;
                    if ((1u << align) > load_width(op)) {
                        vfail(&v, "alignment must not be larger than natural");
                        break;
                    }
                    if (memidx >= m->n_memories) { vfail(&v, "unknown memory"); break; }
                    pop_val(&v, m->memories[memidx].is64 ? VT_I64 : VT_I32);
                    if (!v.failed) {
                        EaValType r = (op == EA_OP_F32_LOAD) ? VT_F32
                                      : (op == EA_OP_F64_LOAD) ? VT_F64
                                      : (op == EA_OP_I64_LOAD ||
                                         (op >= EA_OP_I64_LOAD8_S && op <= EA_OP_I64_LOAD32_U))
                                          ? VT_I64 : VT_I32;
                        push_val(&v, r);
                    }
                    break;
                }
                if (is_store(op)) {
                    uint32_t align = in->imm.pair.a;
                    uint32_t memidx = in->imm.q.c;
                    if ((1u << align) > store_width(op)) {
                        vfail(&v, "alignment must not be larger than natural");
                        break;
                    }
                    if (memidx >= m->n_memories) { vfail(&v, "unknown memory"); break; }
                    EaValType vt2 = (op == EA_OP_F32_STORE) ? VT_F32
                                    : (op == EA_OP_F64_STORE) ? VT_F64
                                    : (op == EA_OP_I64_STORE || op == EA_OP_I64_STORE8 ||
                                       op == EA_OP_I64_STORE16 || op == EA_OP_I64_STORE32)
                                        ? VT_I64 : VT_I32;
                    pop_val(&v, vt2);
                    if (!v.failed) pop_val(&v, m->memories[memidx].is64 ? VT_I64 : VT_I32);
                    break;
                }
                if (op == EA_OP_MEMORY_SIZE || op == EA_OP_MEMORY_GROW ||
                    op == EA_OP_MEMORY_COPY || op == EA_OP_MEMORY_FILL ||
                    op == EA_OP_MEMORY_INIT || op == EA_OP_DATA_DROP) {
                    if (op == EA_OP_MEMORY_INIT) {
                        if (!m->has_data_count) { vfail(&v, "data count section required"); break; }
                        if (in->imm.pair.b >= m->n_datas) { vfail(&v, "unknown data segment"); break; }
                        if (in->imm.pair.a >= m->n_memories) { vfail(&v, "unknown memory"); break; }
                    } else if (op == EA_OP_DATA_DROP) {
                        if (!m->has_data_count) { vfail(&v, "data count section required"); break; }
                        if (in->imm.u32 >= m->n_datas) { vfail(&v, "unknown data segment"); break; }
                    } else if (op == EA_OP_MEMORY_COPY) {
                        if (in->imm.pair.a >= m->n_memories || in->imm.pair.b >= m->n_memories) {
                            vfail(&v, "unknown memory"); break;
                        }
                    } else {
                        if (in->imm.u32 >= m->n_memories) { vfail(&v, "unknown memory"); break; }
                    }
                    if (op == EA_OP_DATA_DROP) break;
                    op_sig(op, &sig);
                    if (op == EA_OP_MEMORY_COPY) {
                        // address width per memory pair
                        EaValType da = m->memories[in->imm.pair.a].is64 ? VT_I64 : VT_I32;
                        pop_val(&v, da); pop_val(&v, da); pop_val(&v, da);
                    } else if (op == EA_OP_MEMORY_FILL) {
                        EaValType da = m->memories[in->imm.u32].is64 ? VT_I64 : VT_I32;
                        pop_val(&v, da); pop_val(&v, VT_I32); pop_val(&v, da);
                    } else if (op == EA_OP_MEMORY_INIT) {
                        EaValType da = m->memories[in->imm.pair.a].is64 ? VT_I64 : VT_I32;
                        pop_val(&v, da); pop_val(&v, VT_I32); pop_val(&v, VT_I32);
                    } else if (op == EA_OP_MEMORY_SIZE) {
                        push_val(&v, m->memories[in->imm.u32].is64 ? VT_I64 : VT_I32);
                    } else if (op == EA_OP_MEMORY_GROW) {
                        EaValType da = m->memories[in->imm.u32].is64 ? VT_I64 : VT_I32;
                        pop_val(&v, da);
                        if (!v.failed) push_val(&v, da);
                    } else {
                        pop_vals(&v, sig.in, sig.n_in);
                        if (!v.failed) push_vals(&v, sig.out, sig.n_out);
                    }
                    break;
                }
                if (!op_sig(op, &sig)) {
                    if ((op & 0xFF00) == EA_OPV_BASE && !m->feat.simd)
                        vfail(&v, "SIMD not enabled");
                    else
                        vfail(&v, "illegal opcode");
                    break;
                }
                pop_vals(&v, sig.in, sig.n_in);
                if (!v.failed) push_vals(&v, sig.out, sig.n_out);
                break;
            }
            }
        }
        if (!v.failed) {
            if (v.csp != 0 || v.sp != ft->n_results ||
                memcmp(v.vals, ft->results, ft->n_results * sizeof(EaValType)) != 0)
                vfail(&v, "type mismatch: function end");
        }
        f->max_stack = v.max_stack;
        f->depths = depths;
        f->reach = reach;
        f->br_targets = br_ar;
        depths = NULL; reach = NULL; br_ar = NULL;
        if (v.failed) {
            if (getenv("EA_VDBG")) {
                fprintf(stderr, "EA_VDBG vfail func %u pc %u/%u: %s (sp=%u csp=%u)\n", fi, cur_pc,
                        f->code.n, v.err ? v.err : "?", v.sp, v.csp);
                for (uint32_t k = 0; k < f->code.n; k++)
                    fprintf(stderr, "   [%u] 0x%02x end=%u else=%d\n", k, f->code.v[k].opcode,
                            f->code.v[k].end_idx, (int)f->code.v[k].else_idx);
            }
            if (err && !*err) {
                const char *msg = v.err ? v.err : "validation failure";
                size_t n = strlen(msg) + 32;
                *err = (char *)ea_malloc(n);
                snprintf(*err, n, "%s", msg);
            }
            if (getenv("EA_VDBG")) fprintf(stderr, "EA_VDBG LOOP-FAIL path: func %u err=%s v.vals=%p\n",
                                           fi, v.err ? v.err : "?", (void*)v.vals);
            free(depths); free(reach); free(br_ar);
            free(v.err);
            free(v.vals);
            free(v.ctrl);
            free(in_refs);
            return 1;
        }
    }
out:
    if (getenv("EA_VDBG")) fprintf(stderr, "EA_VDBG OUT path: rc=%d v.vals=%p\n", rc, (void*)v.vals);
    free(in_refs);
    free(v.vals);
    free(v.ctrl);
    return rc;
}

// ---------------------------------------------------------------- SIMD signatures
static void sig_vv(Sig *s) { s->n_in = 2; s->in[0] = s->in[1] = VT_V128; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_v(Sig *s)  { s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_vi(Sig *s) { s->n_in = 2; s->in[0] = VT_V128; s->in[1] = VT_I32; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_v3(Sig *s) { s->n_in = 3; s->in[0] = s->in[1] = s->in[2] = VT_V128; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_to_i32(Sig *s) { s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_I32; }
static void sig_load(Sig *s)  { s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_load_lane(Sig *s) { s->n_in = 2; s->in[0] = VT_I32; s->in[1] = VT_V128; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_store(Sig *s) { s->n_in = 2; s->in[0] = VT_I32; s->in[1] = VT_V128; s->n_out = 0; }

bool simd_sig(uint32_t op, Sig *s) {
    uint32_t sub = op & 0xFF;
    bool ext = (op & 0xFF00FF) > (EA_OPV_BASE + 0xFF); // relaxed range
    if (ext) {
        switch (op) {
        case EA_OP_F32X4_RELAXED_MADD: case EA_OP_F32X4_RELAXED_NMADD:
        case EA_OP_F64X2_RELAXED_MADD: case EA_OP_F64X2_RELAXED_NMADD:
        case EA_OP_I8X16_RELAXED_LANESELECT: case EA_OP_I16X8_RELAXED_LANESELECT:
        case EA_OP_I32X4_RELAXED_LANESELECT: case EA_OP_I64X2_RELAXED_LANESELECT:
            sig_v3(s);
            return true;
        default:
            sig_vv(s);
            return true;
        }
    }
    switch (sub) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A:
    case 0x5C: case 0x5D:
        sig_load(s);
        return true;
    case 0x0B:
        sig_store(s);
        return true;
    case 0x0C:
        s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x0D: case 0x0E:
        sig_vv(s);
        return true;
    case 0x54: case 0x55: case 0x56: case 0x57:
        sig_load_lane(s);
        return true;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
        sig_store(s);
        return true;
    case 0x0F: case 0x10: case 0x11:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x12:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x13:
        s->n_in = 1; s->in[0] = VT_F32; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x14:
        s->n_in = 1; s->in[0] = VT_F64; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x15: case 0x16: case 0x18: case 0x19: case 0x1B:
        s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_I32;
        return true;
    case 0x1D:
        s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_I64;
        return true;
    case 0x1F:
        s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_F32;
        return true;
    case 0x21:
        s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_F64;
        return true;
    case 0x17: case 0x1A: case 0x1C:
        s->n_in = 2; s->in[0] = VT_V128; s->in[1] = VT_I32; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x1E:
        s->n_in = 2; s->in[0] = VT_V128; s->in[1] = VT_I64; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x20:
        s->n_in = 2; s->in[0] = VT_V128; s->in[1] = VT_F32; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x22:
        s->n_in = 2; s->in[0] = VT_V128; s->in[1] = VT_F64; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x53:
        sig_to_i32(s);
        return true;
    case 0x63: case 0x83: case 0xA3: case 0xC3:
    case 0x64: case 0x84: case 0xA4: case 0xC4:
        sig_to_i32(s);
        return true;
    case 0x52:
        sig_v3(s);
        return true;
    case 0x4D: // not
    case 0x5E: case 0x5F:
    case 0x60: case 0x61: case 0x62:
    case 0x67: case 0x68: case 0x69: case 0x6A:
    case 0x74: case 0x75: case 0x7A: case 0x94:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
    case 0x80: case 0x81:
    case 0x87: case 0x88: case 0x89: case 0x8A:
    case 0xA0: case 0xA1:
    case 0xA7: case 0xA8: case 0xA9: case 0xAA:
    case 0xC0: case 0xC1:
    case 0xC7: case 0xC8: case 0xC9: case 0xCA:
    case 0xE0: case 0xE1: case 0xE3:
    case 0xEC: case 0xED: case 0xEF:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        sig_v(s);
        return true;
    case 0x6B: case 0x6C: case 0x6D:
    case 0x8B: case 0x8C: case 0x8D:
    case 0xAB: case 0xAC: case 0xAD:
    case 0xCB: case 0xCC: case 0xCD:
        sig_vi(s);
        return true;
    default:
        sig_vv(s);
        return true;
    }
}
