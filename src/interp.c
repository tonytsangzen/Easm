// easm interpreter: switch-based execution over pre-decoded instruction lists
#include "easm.h"
#include "opcodes.h"
#include <math.h>
#include <setjmp.h>
#include <stdio.h>

int interp_ensure(EaExec *ex, uint32_t extra) {
    uint64_t need = (uint64_t)ex->sp + extra;
    if (need <= ex->stack_cap) return 0;
    uint64_t cap = ex->stack_cap ? ex->stack_cap : 4096;
    while (cap < need) cap *= 2;
    ex->stack = (WVal *)ea_realloc(ex->stack, cap * sizeof(WVal));
    ex->stack_cap = (uint32_t)cap;
    return 0;
}

#define TRAP(code) do { ea_trap(ex, (code)); return 1; } while (0)

static uint64_t load_width_bytes(uint32_t op) {
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
static uint64_t store_width_bytes(uint32_t op) {
    switch (op) {
    case EA_OP_I32_STORE8: case EA_OP_I64_STORE8: return 1;
    case EA_OP_I32_STORE16: case EA_OP_I64_STORE16: return 2;
    case EA_OP_I32_STORE: case EA_OP_F32_STORE:
    case EA_OP_I64_STORE32: return 4;
    default: return 8;
    }
}
int exec_numeric(EaExec *ex, uint32_t op, WVal *S, uint32_t *spp);
int exec_simd(EaExec *ex, EaInstance *inst, const EaInstr *in, WVal *S, uint32_t *spp);

// ---------------------------------------------------------------- runtime ctrl frame
typedef struct {
    uint32_t height;
    uint32_t arity;     // br label arity (loop: params)
    uint32_t rarity;    // result arity on natural fallthrough
    uint32_t pc_end;
    uint32_t pc_else;
    uint32_t block_idx;
    uint8_t is_loop;
} RCtl;

#define MAX_CTRL 4096

// execute function; args are the top n_params values on the stack.
// on success params replaced in place by results.
int ea_interp_exec_function(EaExec *ex, EaFuncInst *fi) {
    if (ex->depth >= ex->max_depth) TRAP(TRAP_STACK_EXHAUSTED);
    EaInstance *inst = fi->inst;
    EaModule *m = inst->module;
    EaFunc *f = (EaFunc *)fi->code;
    EaFuncType *ft = fi->type;
    uint32_t n_params = ft->n_params;
    uint32_t n_results = ft->n_results;

    uint32_t base = ex->sp - n_params; // locals live at [base, base+n_locals)
    uint32_t n_locals = f->n_locals;
    if (interp_ensure(ex, n_locals + f->max_stack + 8) != 0)
        TRAP(TRAP_STACK_EXHAUSTED);
    // shift params down? params are already at [base..base+n_params) — locals region overlaps
    // zero-init non-param locals
    for (uint32_t i = n_params; i < n_locals; i++)
        memset(&ex->stack[base + i], 0, sizeof(WVal));
    ex->sp = base + n_locals;

    WVal *S = ex->stack;
    uint32_t sp = ex->sp;
    uint32_t hoff = base + n_locals; // validator heights are relative to operand base
    uint32_t ctl_cap = 32;
    RCtl *ctl = (RCtl *)ea_malloc(ctl_cap * sizeof(RCtl));
    uint32_t csp = 0;
    uint32_t pc = 0;
    int result = 0;

#define STACK_GUARD(n) do { if (interp_ensure(ex, (uint32_t)(n)) != 0) TRAP(TRAP_STACK_EXHAUSTED); S = ex->stack; } while (0)

    EaInstr *code = f->code.v;
    uint32_t n_instr = f->code.n;
#define EA_TRACE() do { if (getenv("EA_IDBG3") && ex->depth == 0) fprintf(stderr, "pc=%u op=%02x sp=%u base=%u csp=%u\n", pc, code[pc].opcode, sp, base, csp); } while (0)
    // synthetic function frame: br to the function label behaves like return
    ctl[0].height = base;
    ctl[0].arity = n_results;
    ctl[0].rarity = n_results;
    ctl[0].pc_end = n_instr;
    ctl[0].pc_else = UINT32_MAX;
    ctl[0].block_idx = UINT32_MAX;
    ctl[0].is_loop = 0;
    csp = 1;
#define CTL_PUSH() do { \
    if (csp == ctl_cap) { ctl_cap *= 2; ctl = (RCtl *)ea_realloc(ctl, ctl_cap * sizeof(RCtl)); } \
} while (0)
    while (pc < n_instr) {
        EA_TRACE();
        EaInstr *in = &code[pc];
        uint32_t op = in->opcode;
        switch (op) {
        case EA_OP_UNREACHABLE: TRAP(TRAP_UNREACHABLE);
        case EA_OP_NOP: pc++; break;
        case EA_OP_BLOCK: case EA_OP_LOOP: {
            CTL_PUSH();
            ctl[csp].height = in->height + hoff;
            ctl[csp].arity = in->arity_out;
            ctl[csp].rarity = in->arity_res;
            ctl[csp].pc_end = in->end_idx;
            ctl[csp].pc_else = UINT32_MAX;
            ctl[csp].block_idx = pc;
            ctl[csp].is_loop = op == EA_OP_LOOP;
            csp++;
            pc++;
            break;
        }
        case EA_OP_IF: {
            uint32_t cond = S[--sp].i32;
            CTL_PUSH();
            ctl[csp].height = in->height + hoff;
            ctl[csp].arity = in->arity_out;
            ctl[csp].rarity = in->arity_res;
            ctl[csp].pc_end = in->end_idx;
            ctl[csp].pc_else = in->else_idx;
            ctl[csp].block_idx = pc;
            ctl[csp].is_loop = 0;
            csp++;
            if (!cond) {
                pc = (in->else_idx != UINT32_MAX) ? in->else_idx + 1 : in->end_idx;
            } else {
                pc++;
            }
            break;
        }
        case EA_OP_ELSE:
            // taken branch finished: jump to end
            sp = ctl[csp - 1].height + ctl[csp - 1].rarity;
            pc = ctl[csp - 1].pc_end;
            break;
        case EA_OP_END:
            if (csp > 1) {
                sp = ctl[csp - 1].height + ctl[csp - 1].rarity;
                csp--;
            } else {
                csp = 0; // function-final end: results stay on stack for the tail
            }
            pc++;
            break;
        case EA_OP_BR: {
            uint32_t l = in->imm.u32;
            RCtl *t = &ctl[csp - 1 - l];
            uint32_t arity = t->arity;
            uint32_t h = t->height;
            if (arity && h != sp - arity) memmove(&S[h], &S[sp - arity], arity * sizeof(WVal));
            sp = h + arity;
            pc = t->is_loop ? t->block_idx + 1 : t->pc_end;
            csp -= l;
            break;
        }
        case EA_OP_BR_IF: {
            uint32_t cond = S[--sp].i32;
            if (cond) {
                uint32_t l = in->imm.u32;
                RCtl *t = &ctl[csp - 1 - l];
                uint32_t arity = t->arity;
                uint32_t h = t->height;
                if (arity && h != sp - arity) memmove(&S[h], &S[sp - arity], arity * sizeof(WVal));
                sp = h + arity;
                pc = t->is_loop ? t->block_idx + 1 : t->pc_end;
                csp -= l;
            } else {
                pc++;
            }
            break;
        }
        case EA_OP_BR_TABLE: {
            uint32_t idx = S[--sp].i32;
            const uint32_t *tg = &f->code.pool[in->imm.pair.a];
            uint32_t n = in->imm.pair.b;
            uint32_t l = idx < n ? tg[idx] : tg[n];
            RCtl *t = &ctl[csp - 1 - l];
            uint32_t arity = t->arity;
            uint32_t h = t->height;
            if (arity && h != sp - arity) memmove(&S[h], &S[sp - arity], arity * sizeof(WVal));
            sp = h + arity;
            pc = t->is_loop ? t->block_idx + 1 : t->pc_end;
            csp -= l;
            break;
        }
        case EA_OP_RETURN: {
            if (n_results && base != sp - n_results)
                memmove(&S[base], &S[sp - n_results], n_results * sizeof(WVal));
            sp = base + n_results;
            goto done;
        }
        case EA_OP_CALL: {
            uint32_t ci = in->imm.u32;
            EaFuncInst *callee = &inst->funcs[ci];
            if (callee->is_jit) {
                ex->sp = sp; // sync before the JIT call reads args at ex->sp
                if (ea_jit_call(ex, callee) != 0) goto trapped;
                S = ex->stack; sp = ex->sp;
            } else if (callee->is_host) {
                uint32_t np = callee->type->n_params;
                WVal la[16], lr[16];
                for (uint32_t i = 0; i < np && i < 16; i++) la[i] = S[sp - np + i];
                sp -= np;
                memset(lr, 0, sizeof(lr));
                if (callee->host_fn(callee->host_user, la, lr) != 0)
                    TRAP(TRAP_HOST);
                uint32_t nr = callee->type->n_results;
                for (uint32_t i = 0; i < nr; i++) S[sp + i] = lr[i];
                sp += nr;
            } else {
                ex->sp = sp;
                ex->depth++;
                int rc = ea_interp_exec_function(ex, callee);
                ex->depth--;
                S = ex->stack;
                sp = ex->sp;
                if (rc != 0) { result = rc; goto done_err; }
            }
            pc++;
            break;
        }
        case EA_OP_RETURN_CALL: {
            uint32_t ci = in->imm.u32;
            EaFuncInst *callee = &inst->funcs[ci];
            if (callee->is_jit) {
                ex->sp = sp;
                if (ea_jit_call(ex, callee) != 0) goto trapped;
                S = ex->stack; sp = ex->sp;
            } else if (callee->is_host) {
                uint32_t np = callee->type->n_params;
                WVal la[16], lr[16];
                for (uint32_t i = 0; i < np && i < 16; i++) la[i] = S[sp - np + i];
                sp -= np;
                memset(lr, 0, sizeof(lr));
                if (callee->host_fn(callee->host_user, la, lr) != 0)
                    TRAP(TRAP_HOST);
                uint32_t nr = callee->type->n_results;
                for (uint32_t i = 0; i < nr; i++) S[sp + i] = lr[i];
                sp += nr;
            } else {
                // true tail jump: recycle this frame for the interpreted callee
                uint32_t np2 = callee->type->n_params;
                if (np2 && base != sp - np2)
                    memmove(&S[base], &S[sp - np2], np2 * sizeof(WVal));
                sp = base + np2;
                ex->sp = sp;
                inst = callee->inst;
                m = inst->module;
                f = (EaFunc *)callee->code;
                ft = callee->type;
                n_params = ft->n_params;
                n_results = ft->n_results;
                base = sp - n_params;
                n_locals = f->n_locals;
                if (interp_ensure(ex, n_locals + f->max_stack + 8) != 0)
                    TRAP(TRAP_STACK_EXHAUSTED);
                for (uint32_t i2 = n_params; i2 < n_locals; i2++)
                    memset(&ex->stack[base + i2], 0, sizeof(WVal));
                ex->sp = base + n_locals;
                S = ex->stack;
                sp = ex->sp;
                hoff = base + n_locals;
                code = f->code.v;
                n_instr = f->code.n;
                ctl[0].height = base;
                ctl[0].arity = n_results;
                ctl[0].rarity = n_results;
                ctl[0].pc_end = n_instr;
                ctl[0].pc_else = UINT32_MAX;
                ctl[0].block_idx = UINT32_MAX;
                ctl[0].is_loop = 0;
                csp = 1;
                pc = 0;
                continue;
            }
            // jit / host callees: callee results become this frame's results
            if (n_results && base != sp - n_results)
                memmove(&S[base], &S[sp - n_results], n_results * sizeof(WVal));
            sp = base + n_results;
            goto done;
        }
        case EA_OP_CALL_INDIRECT: {
            uint32_t ti = in->imm.pair.b;
            uint32_t idx = S[--sp].i32;
            EaTableInst *tab = &inst->tables[ti];
            if (idx >= tab->size) TRAP(TRAP_UNDEF_ELEM);
            EaFuncInst *callee = (EaFuncInst *)tab->elems[idx].ref;
            if (!callee) {
                ex->trap = TRAP_UNINIT_ELEM;
                snprintf(ex->trap_msg, sizeof(ex->trap_msg), "uninitialized element %u", idx);
                if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1);
                TRAP(TRAP_UNINIT_ELEM);
            }
            EaFuncType *want = &m->types[in->imm.pair.a].func;
            if (callee->type->n_params != want->n_params ||
                memcmp(callee->type->params, want->params, want->n_params * sizeof(EaValType)) != 0 ||
                callee->type->n_results != want->n_results ||
                memcmp(callee->type->results, want->results, want->n_results * sizeof(EaValType)) != 0)
                TRAP(TRAP_INDIRECT_TYPE);
            if (callee->is_jit) {
                ex->sp = sp; // sync before the JIT call reads args at ex->sp
                if (ea_jit_call(ex, callee) != 0) goto trapped;
                S = ex->stack; sp = ex->sp;
            } else if (callee->is_host) {
                uint32_t np = callee->type->n_params;
                WVal la[16], lr[16];
                for (uint32_t i = 0; i < np && i < 16; i++) la[i] = S[sp - np + i];
                sp -= np;
                memset(lr, 0, sizeof(lr));
                if (callee->host_fn(callee->host_user, la, lr) != 0)
                    TRAP(TRAP_HOST);
                uint32_t nr = callee->type->n_results;
                for (uint32_t i = 0; i < nr; i++) S[sp + i] = lr[i];
                sp += nr;
            } else {
                ex->sp = sp;
                ex->depth++;
                int rc = ea_interp_exec_function(ex, callee);
                ex->depth--;
                S = ex->stack;
                sp = ex->sp;
                if (rc != 0) { result = rc; goto done_err; }
            }
            pc++;
            break;
        }
        case EA_OP_RETURN_CALL_INDIRECT: {
            uint32_t ti = in->imm.pair.b;
            uint32_t idx = S[--sp].i32;
            EaTableInst *tab = &inst->tables[ti];
            if (idx >= tab->size) TRAP(TRAP_UNDEF_ELEM);
            EaFuncInst *callee = (EaFuncInst *)tab->elems[idx].ref;
            if (!callee) {
                ex->trap = TRAP_UNINIT_ELEM;
                snprintf(ex->trap_msg, sizeof(ex->trap_msg), "uninitialized element %u", idx);
                if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1);
                TRAP(TRAP_UNINIT_ELEM);
            }
            EaFuncType *want = &m->types[in->imm.pair.a].func;
            if (callee->type->n_params != want->n_params ||
                memcmp(callee->type->params, want->params, want->n_params * sizeof(EaValType)) != 0 ||
                callee->type->n_results != want->n_results ||
                memcmp(callee->type->results, want->results, want->n_results * sizeof(EaValType)) != 0)
                TRAP(TRAP_INDIRECT_TYPE);
            if (callee->is_jit) {
                ex->sp = sp;
                if (ea_jit_call(ex, callee) != 0) goto trapped;
                S = ex->stack; sp = ex->sp;
            } else if (callee->is_host) {
                uint32_t np = callee->type->n_params;
                WVal la[16], lr[16];
                for (uint32_t i = 0; i < np && i < 16; i++) la[i] = S[sp - np + i];
                sp -= np;
                memset(lr, 0, sizeof(lr));
                if (callee->host_fn(callee->host_user, la, lr) != 0)
                    TRAP(TRAP_HOST);
                uint32_t nr = callee->type->n_results;
                for (uint32_t i = 0; i < nr; i++) S[sp + i] = lr[i];
                sp += nr;
            } else {
                ex->sp = sp;
                ex->depth++;
                int rc = ea_interp_exec_function(ex, callee);
                ex->depth--;
                S = ex->stack;
                sp = ex->sp;
                if (rc != 0) { result = rc; goto done_err; }
            }
            if (n_results && base != sp - n_results)
                memmove(&S[base], &S[sp - n_results], n_results * sizeof(WVal));
            sp = base + n_results;
            goto done;
        }
        case EA_OP_DROP: sp--; pc++; break;
        case EA_OP_SELECT: {
            uint32_t c = S[--sp].i32;
            WVal b = S[--sp];
            WVal a = S[--sp];
            S[sp++] = c ? a : b;
            pc++;
            break;
        }
        case EA_OP_SELECT_T: {
            uint32_t c = S[--sp].i32;
            WVal b = S[--sp];
            WVal a = S[--sp];
            S[sp++] = c ? a : b;
            pc++;
            break;
        }
        case EA_OP_LOCAL_GET: S[sp++] = S[base + in->imm.u32]; pc++; break;
        case EA_OP_LOCAL_SET: S[base + in->imm.u32] = S[--sp]; pc++; break;
        case EA_OP_LOCAL_TEE: S[base + in->imm.u32] = S[sp - 1]; pc++; break;
        case EA_OP_GLOBAL_GET: S[sp++] = inst->globals[in->imm.u32]; pc++; break;
        case EA_OP_GLOBAL_SET: inst->globals[in->imm.u32] = S[--sp]; pc++; break;
        case EA_OP_TABLE_GET: {
            EaTableInst *t = &inst->tables[in->imm.u32];
            uint64_t i = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (i >= t->size) TRAP(TRAP_OOB_TABLE);
            S[sp++] = t->elems[i];
            pc++;
            break;
        }
        case EA_OP_TABLE_SET: {
            EaTableInst *t = &inst->tables[in->imm.u32];
            WVal v = S[--sp];
            uint64_t i = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (i >= t->size) TRAP(TRAP_OOB_TABLE);
            t->elems[i] = v;
            pc++;
            break;
        }
        case EA_OP_TABLE_SIZE: {
            { EaTableInst *t = &inst->tables[in->imm.u32];
              if (t->is64) S[sp++].i64 = t->size; else S[sp++].i32 = (uint32_t)t->size; }
            pc++;
            break;
        }
        case EA_OP_TABLE_GROW: {
            EaTableInst *t = &inst->tables[in->imm.u32];
            uint64_t delta = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            WVal init = S[--sp];
            uint64_t old = t->size;
            uint64_t newsz = old + delta;
            if (newsz > t->max || newsz > (1ull << 32)) {
                if (t->is64) S[sp++].i64 = -1; else S[sp++].i32 = 0xFFFFFFFFu;
            } else {
                t->elems = (WVal *)ea_realloc(t->elems, (newsz ? newsz : 1) * sizeof(WVal));
                for (uint64_t i = old; i < newsz; i++) t->elems[i] = init;
                t->size = newsz;
                if (t->is64) S[sp++].i64 = old; else S[sp++].i32 = (uint32_t)old;
            }
            pc++;
            break;
        }
        case EA_OP_TABLE_FILL: {
            EaTableInst *t = &inst->tables[in->imm.u32];
            uint64_t n = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            WVal v = S[--sp];
            uint64_t d = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (d + n > t->size) TRAP(TRAP_OOB_TABLE);
            for (uint64_t i = 0; i < n; i++) t->elems[d + i] = v;
            pc++;
            break;
        }
        case EA_OP_TABLE_COPY: {
            uint32_t dt = in->imm.pair.a, st = in->imm.pair.b;
            EaTableInst *td = &inst->tables[dt], *ts = &inst->tables[st];
            uint64_t n = td->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t s2 = ts->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t d = td->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (d + n > td->size || s2 + n > ts->size) TRAP(TRAP_OOB_TABLE);
            memmove(&td->elems[d], &ts->elems[s2], (size_t)n * sizeof(WVal));
            pc++;
            break;
        }
        case EA_OP_TABLE_INIT: {
            uint32_t ti = in->imm.pair.a, ei = in->imm.pair.b;
            EaTableInst *t = &inst->tables[ti];
            EaElem *e = &m->elems[ei];
            uint64_t n = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t s2 = (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t d = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            {
                uint64_t ilen = inst->elem_alive[in->imm.pair.b] ? e->n_items : 0;
                if (s2 + n > ilen) TRAP(TRAP_OOB_TABLE);
            }
            if (d + n > t->size) TRAP(TRAP_OOB_TABLE);
            for (uint32_t i = 0; i < n; i++) {
                WVal v;
                if (e->items) {
                    if (ea_eval_const_expr(inst, m, &e->items[s2 + i], &v, e->ref_type) != 0)
                        TRAP(TRAP_OOB_TABLE);
                } else {
                    v.ref = &inst->funcs[e->func_idx[s2 + i]];
                }
                t->elems[d + i] = v;
            }
            pc++;
            break;
        }
        case EA_OP_ELEM_DROP:
            inst->elem_alive[in->imm.u32] = 0;
            pc++;
            break;
        // ---------------- loads
        case EA_OP_I32_LOAD: case EA_OP_I64_LOAD: case EA_OP_F32_LOAD: case EA_OP_F64_LOAD:
        case EA_OP_I32_LOAD8_S: case EA_OP_I32_LOAD8_U:
        case EA_OP_I32_LOAD16_S: case EA_OP_I32_LOAD16_U:
        case EA_OP_I64_LOAD8_S: case EA_OP_I64_LOAD8_U:
        case EA_OP_I64_LOAD16_S: case EA_OP_I64_LOAD16_U:
        case EA_OP_I64_LOAD32_S: case EA_OP_I64_LOAD32_U: {
            EaMemInst *mem = &inst->memories[in->imm.q.c];
            uint64_t ea = (mem->is64 ? (uint64_t)S[--sp].i64
                          : (uint64_t)(uint32_t)S[--sp].i32) + in->imm.pair.b;
            uint64_t w = load_width_bytes(op);
            if (getenv("EA_LDBG")) fprintf(stderr, "LOAD op=%x ea=%llu w=%llu size=%llu\n", op, (unsigned long long)ea, (unsigned long long)w, (unsigned long long)mem->size);
            if (w > mem->size || ea > mem->size - w) TRAP(TRAP_OOB_MEMORY);
            uint8_t *p = mem->base + ea;
            switch (op) {
            case EA_OP_I32_LOAD: memcpy(&S[sp].i32, p, 4); break;
            case EA_OP_I64_LOAD: memcpy(&S[sp].i64, p, 8); break;
            case EA_OP_F32_LOAD: memcpy(&S[sp].f32, p, 4); break;
            case EA_OP_F64_LOAD: memcpy(&S[sp].f64, p, 8); break;
            case EA_OP_I32_LOAD8_S: S[sp].i32 = (uint32_t)(int32_t)(int8_t)*p; break;
            case EA_OP_I32_LOAD8_U: S[sp].i32 = *p; break;
            case EA_OP_I32_LOAD16_S: { int16_t v; memcpy(&v, p, 2); S[sp].i32 = (uint32_t)(int32_t)v; break; }
            case EA_OP_I32_LOAD16_U: { uint16_t v; memcpy(&v, p, 2); S[sp].i32 = v; break; }
            case EA_OP_I64_LOAD8_S: S[sp].i64 = (uint64_t)(int64_t)(int8_t)*p; break;
            case EA_OP_I64_LOAD8_U: S[sp].i64 = *p; break;
            case EA_OP_I64_LOAD16_S: { int16_t v; memcpy(&v, p, 2); S[sp].i64 = (uint64_t)(int64_t)v; break; }
            case EA_OP_I64_LOAD16_U: { uint16_t v; memcpy(&v, p, 2); S[sp].i64 = v; break; }
            case EA_OP_I64_LOAD32_S: { int32_t v; memcpy(&v, p, 4); S[sp].i64 = (uint64_t)(int64_t)v; break; }
            case EA_OP_I64_LOAD32_U: { uint32_t v; memcpy(&v, p, 4); S[sp].i64 = v; break; }
            }
            sp++;
            pc++;
            break;
        }
        // ---------------- stores
        case EA_OP_I32_STORE: case EA_OP_I64_STORE: case EA_OP_F32_STORE: case EA_OP_F64_STORE:
        case EA_OP_I32_STORE8: case EA_OP_I32_STORE16:
        case EA_OP_I64_STORE8: case EA_OP_I64_STORE16: case EA_OP_I64_STORE32: {
            EaMemInst *mem = &inst->memories[in->imm.q.c];
            WVal v = S[--sp];
            uint64_t ea = (mem->is64 ? (uint64_t)S[--sp].i64
                          : (uint64_t)(uint32_t)S[--sp].i32) + in->imm.pair.b;
            uint64_t w = store_width_bytes(op);
            if (w > mem->size || ea > mem->size - w) TRAP(TRAP_OOB_MEMORY);
            uint8_t *p = mem->base + ea;
            switch (op) {
            case EA_OP_I32_STORE: memcpy(p, &v.i32, 4); break;
            case EA_OP_I64_STORE: memcpy(p, &v.i64, 8); break;
            case EA_OP_F32_STORE: memcpy(p, &v.f32, 4); break;
            case EA_OP_F64_STORE: memcpy(p, &v.f64, 8); break;
            case EA_OP_I32_STORE8: case EA_OP_I64_STORE8: *p = (uint8_t)v.i32; break;
            case EA_OP_I32_STORE16: case EA_OP_I64_STORE16: { uint16_t x = (uint16_t)v.i32; memcpy(p, &x, 2); break; }
            case EA_OP_I64_STORE32: { uint32_t x = (uint32_t)v.i64; memcpy(p, &x, 4); break; }
            }
            pc++;
            break;
        }
        case EA_OP_MEMORY_SIZE:
            S[sp++].i32 = (uint32_t)inst->memories[in->imm.u32].pages;
            pc++;
            break;
        case EA_OP_MEMORY_GROW: {
            EaMemInst *mem = &inst->memories[in->imm.u32];
            uint32_t delta = S[--sp].i32;
            uint64_t old;
            if (ea_grow_memory(mem, delta, &old)) S[sp++].i32 = (uint32_t)old;
            else S[sp++].i32 = 0xFFFFFFFFu;
            pc++;
            break;
        }
        case EA_OP_MEMORY_INIT: {
            EaMemInst *mem = &inst->memories[in->imm.pair.a];
            EaData *d = &m->datas[in->imm.pair.b];
            uint64_t n = mem->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t s2 = (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t d2 = mem->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            {
                uint64_t dlen = inst->data_alive[in->imm.pair.b] ? d->data_len : 0;
                if (s2 + n > dlen) TRAP(TRAP_OOB_MEMORY);
            }
            if (n > mem->size || d2 > mem->size - n) TRAP(TRAP_OOB_MEMORY);
            if (n) memcpy(mem->base + d2, m->owned_bytes + d->data_off + s2, n);
            pc++;
            break;
        }
        case EA_OP_DATA_DROP:
            inst->data_alive[in->imm.u32] = 0;
            pc++;
            break;
        case EA_OP_MEMORY_COPY: {
            EaMemInst *dst = &inst->memories[in->imm.pair.a];
            EaMemInst *src = &inst->memories[in->imm.pair.b];
            uint64_t n = dst->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t s2 = src->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t d2 = dst->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (n > src->size || s2 > src->size - n) TRAP(TRAP_OOB_MEMORY);
            if (n > dst->size || d2 > dst->size - n) TRAP(TRAP_OOB_MEMORY);
            if (n) memmove(dst->base + d2, src->base + s2, n);
            pc++;
            break;
        }
        case EA_OP_MEMORY_FILL: {
            EaMemInst *mem = &inst->memories[in->imm.u32];
            uint32_t n = S[--sp].i32;
            uint8_t b = (uint8_t)S[--sp].i32;
            uint32_t d2 = S[--sp].i32;
            if ((uint64_t)d2 + n > mem->size) TRAP(TRAP_OOB_MEMORY);
            if (n) memset(mem->base + d2, b, n);
            pc++;
            break;
        }
        case EA_OP_I32_CONST: S[sp++].i32 = in->imm.u32; pc++; break;
        case EA_OP_I64_CONST: S[sp++].i64 = in->imm.u64; pc++; break;
        case EA_OP_F32_CONST: S[sp++].f32 = in->imm.f32; pc++; break;
        case EA_OP_F64_CONST: S[sp++].f64 = in->imm.f64; pc++; break;
        case EA_OP_REF_NULL: S[sp++].ref = NULL; pc++; break;
        case EA_OP_REF_IS_NULL: sp--; S[sp].i32 = (S[sp].ref == NULL); sp++; pc++; break;
        case EA_OP_REF_FUNC: S[sp++].ref = &inst->funcs[in->imm.u32]; pc++; break;
        case EA_OP_REF_EQ: {
            void *b = S[--sp].ref;
            void *a = S[--sp].ref;
            S[sp++].i32 = a == b;
            pc++;
            break;
        }
        default: {
            int r = exec_numeric(ex, op, S, &sp);
            if (r > 0) { pc++; break; }
            if (r < 0) goto trapped;
            r = exec_simd(ex, inst, in, S, &sp);
            if (r > 0) { pc++; break; }
            if (r < 0) goto trapped;
            TRAP(TRAP_INDIRECT_CALL);
        }
        }
    }
    // fell off the end without RETURN: function frame result
    {
        if (n_results && base != sp - n_results)
            memmove(&S[base], &S[sp - n_results], n_results * sizeof(WVal));
        sp = base + n_results;
    }
done:
    ex->sp = sp;
    free(ctl);
    return result;
done_err:
    ex->sp = sp;
    free(ctl);
    return result;
trapped:
    result = 1;
    goto done_err;
#undef STACK_GUARD
}
