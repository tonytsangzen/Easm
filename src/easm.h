// easm - embed WebAssembly runtime
// aarch64-first: 1:1 instruction-level baseline translation + interpreter fallback.
//
// Core data structures shared by decoder, validator, interpreter and JIT.
#ifndef EASM_H
#define EASM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- utilities
void *ea_malloc(size_t n);
void *ea_realloc(void *p, size_t n);
void *ea_zalloc(size_t n);
char *ea_strndup(const char *s, size_t n);
void ea_fatal(const char *msg); // abort() with message

#define EA_MAX_PAGE_BYTES 65536u

// ---------------------------------------------------------------- valtype
typedef enum {
    VT_I32 = 0, VT_I64 = 1, VT_F32 = 2, VT_F64 = 3,
    VT_V128 = 4,
    VT_FUNCREF = 5, VT_EXTERNREF = 6,
    // extended (3.0) reference/heap types appear only inside validated info
    VT_ANYREF = 7,       // generic "some ref" bucket used by validator
    VT_BOTTOM = 8,       // validation-only bottom type
} EaValType;

static inline bool ea_is_ref(EaValType t) {
    return t == VT_FUNCREF || t == VT_EXTERNREF || t == VT_ANYREF;
}
static inline bool ea_is_num(EaValType t) {
    return t <= VT_V128;
}
const char *ea_vt_name(EaValType t);

// ---------------------------------------------------------------- trap codes
typedef enum {
    TRAP_NONE = 0,
    TRAP_UNREACHABLE,
    TRAP_DIV_BY_ZERO,
    TRAP_INT_OVERFLOW,
    TRAP_INVALID_CONV,
    TRAP_OOB_MEMORY,
    TRAP_OOB_TABLE,
    TRAP_UNDEF_ELEM,     // call_indirect out of table bounds
    TRAP_UNINIT_ELEM,    // call_indirect on null
    TRAP_INDIRECT_TYPE,  // call_indirect signature mismatch
    TRAP_NULL_DEREF,     // ref.as_non_null / call_ref on null
    TRAP_STACK_EXHAUSTED,
    TRAP_INDIRECT_CALL,  // generic indirect call failure
    TRAP_HOST,           // host import signalled an error
} EaTrap;

const char *ea_trap_msg(EaTrap t); // spec-style message text

// ---------------------------------------------------------------- values
typedef union {
    uint64_t i64;
    int64_t  s64;
    uint32_t i32;
    int32_t  s32;
    float    f32;
    double   f64;
    void    *ref;
    uint8_t  v128[16];
} WVal;

typedef struct { uint8_t lane_type; uint16_t lanes[8]; } EaV128Packed; // helper for tests

// ---------------------------------------------------------------- function type
typedef struct {
    uint32_t n_params, n_results;
    EaValType *params;    // n_params
    EaValType *results;   // n_results
} EaFuncType;

// composite type kinds (function-references / GC, minimal support)
typedef enum { CT_FUNC = 0, CT_STRUCT, CT_ARRAY } EaCompKind;

typedef struct {
    EaCompKind kind;
    EaFuncType func;   // valid when kind == CT_FUNC
} EaType;            // module type space entry

// blocktype immediate
typedef struct {
    uint8_t kind;      // 0: empty, 1: single valtype, 2: type index
    uint8_t vt;        // valtype when kind==1
    uint32_t type_idx; // when kind==2
} EaBlockType;

// ---------------------------------------------------------------- module structs
// pre-decoded instruction
typedef struct {
    uint32_t opcode;      // unified EA_OP_*
    uint32_t imm_off;     // byte offset of immediates within body
    union {
        uint32_t u32;         // generic single immediate
        uint64_t u64;
        struct { uint32_t a, b; } pair;      // call_indirect (type,table), memarg, table.copy...
        struct { uint32_t a, b, c, d; } q;   // memarg lane / extra (memidx in c)
        uint8_t bytes[16];                   // v128.const / i8x16.shuffle payload
        float f32;
        double f64;
        EaBlockType bt;
    } imm;
    // control metadata (structured ops; filled during decode/validate)
    uint32_t end_idx;     // matching `end` instruction index (block/loop/if)
    uint32_t else_idx;    // matching `else` (if), or UINT32_MAX
    uint32_t height;      // operand stack height at block entry incl. params (validator)
    uint8_t  is_loop;
    uint16_t arity_out;   // label arity: values carried by br to this label
    uint16_t arity_in;    // params of the block (branch-in arity for else)
    uint16_t arity_res;   // result arity on natural fallthrough (block results)
} EaInstr;

// pre-decoded instruction list (body or const-expr)
typedef struct {
    EaInstr *v;
    uint32_t n;
    uint32_t *pool;    // shared immediates (br_table targets, v128/shuffle bytes)
    uint32_t pool_n;
} InsList;

typedef struct {
    char *name;
    uint8_t kind;
    uint32_t idx;
} EaExport;

typedef struct {
    EaValType ref_type;  // element type
    uint32_t min, max;   // max == UINT32_MAX when absent
    bool has_max;
    bool has_init;       // function-references table init expr
    InsList init;
    bool is64;           // table64 (memory64 proposal)
} EaTable;

typedef struct {
    uint64_t min, max;   // pages
    bool has_max, is64, shared, has_custom_page;
    uint64_t page_size;
} EaMemory;

typedef struct {
    char *module;  // import module name
    char *name;    // import field name
    uint8_t kind;  // EAK_*
    uint32_t idx;  // type idx (func) / tag type idx
    // descriptors for non-func imports
    EaTable table;
    EaMemory memory;
    struct { EaValType type; bool mutable_; } global;
} EaImport;


typedef struct {
    EaValType type;
    bool mutable_;
    InsList init;        // init expression (pre-decoded)
} EaGlobal;

typedef enum { SEG_ACTIVE = 0, SEG_PASSIVE = 1, SEG_DECLARATIVE = 2 } EaSegMode;

typedef struct {
    EaSegMode mode;
    uint32_t table_idx;      // active elem
    InsList offset;          // active: offset expr
    uint32_t n_items;
    EaValType ref_type;
    uint32_t *func_idx;      // item is a plain func index (MVP encoding)
    InsList *items;          // per-item exprs (when func_idx == NULL)
} EaElem;

typedef struct {
    EaSegMode mode;
    uint32_t mem_idx;
    InsList offset;          // active: offset expr
    uint32_t data_off, data_len; // into module binary (bytes)
} EaData;

typedef struct {
    uint32_t type_idx;   // tag signature
} EaTag;

typedef struct {
    uint32_t type_idx;
    uint32_t n_locals;          // total expanded local count (params + locals)
    uint32_t n_declared_locals;
    EaValType *locals;          // expanded, n_locals
    const uint8_t *body;        // pointer into module binary (post locals decl)
    uint32_t body_len;
    InsList code;               // pre-decoded instructions
    uint32_t max_stack;         // max operand stack height (validator)
    uint16_t *depths;           // per-instruction operand depth (validator, JIT input)
    uint8_t *reach;             // per-instruction reachability (validator)
    uint16_t *br_targets;       // per-instruction br arity for unreachable regions (JIT)
    uint32_t code_off;          // offset of body within module binary
    // execution hooks
    void *jit_code;             // aarch64 entry (NULL unless JIT compiled)
    uint32_t n_calls;           // profiling counter
} EaFunc;

typedef enum {
    EAK_FUNC = 0, EAK_TABLE = 1, EAK_MEMORY = 2, EAK_GLOBAL = 3, EAK_TAG = 4,
} EaExternKind;

typedef struct {
    const uint8_t *bytes;
    size_t len;
    char *path; // optional source name
} EaBytes;

typedef struct {
    // sections
    uint32_t n_types; EaType *types;
    uint32_t n_imports; EaImport *imports;
    // counts include imported entries
    uint32_t n_funcs; EaFunc *funcs;
    uint32_t n_tables; EaTable *tables;
    uint32_t n_memories; EaMemory *memories;
    uint32_t n_globals_def; EaGlobal *globals_def; // defined globals only
    uint32_t n_exports; EaExport *exports;
    uint32_t n_elems; EaElem *elems;
    uint32_t n_datas; EaData *datas;
    uint32_t n_tags; EaTag *tags;
    uint32_t start_func;
    bool has_start;
    // import split info
    uint32_t n_imp_funcs, n_imp_tables, n_imp_memories, n_imp_globals, n_imp_tags;
    bool has_data_count; uint32_t data_count;
    // feature flags used at decode/validate time
    struct {
        bool simd, bulk_memory, reference_types, sign_ext, nontrap_convs, multi_value,
             tail_call, typed_funcref, exceptions, memory64, multi_memory, threads,
             relaxed_simd, gc, wide_arithmetic;
    } feat;
    // original binary (kept alive; funcs point into it)
    uint8_t *owned_bytes; size_t owned_len;
} EaModule;

// ---------------------------------------------------------------- decode/validate
// decode module from bytes. Returns 0 on success, negative on malformed (message in *err).
int ea_decode_module(EaModule *m, const uint8_t *bytes, size_t len, char **err);
void ea_module_free(EaModule *m);
// validate module. Returns 0 on success, negative on validation error.
int ea_validate_module(EaModule *m, char **err);

// Evaluate a constant expression (globals init / offsets / elem items) over a pre-decoded list.
// Returns 0 on success.
struct EaInstance;
int ea_eval_const_expr(struct EaInstance *inst, EaModule *m,
                       const InsList *code, WVal *out, EaValType expect);

// ---------------------------------------------------------------- instance
typedef struct EaFuncInst {
    EaFuncType *type;
    struct EaInstance *inst; // owning instance (NULL for host functions)
    uint32_t func_idx;       // index within owning module
    // interpreted functions: code points to EaFunc of inst->module
    void *code;
    // jit entry (aarch64) when is_jit
    bool is_jit;
    void *jit_entry;
    // host imports
    bool is_host;
    int (*host_fn)(void *user, const WVal *args, WVal *results);
    void *host_user;
} EaFuncInst;

typedef struct EaTableInst {
    EaValType ref_type;
    WVal *elems;
    uint64_t size, max;
    bool has_max;
    bool is64;
} EaTableInst;

typedef struct EaMemInst {
    uint8_t *base;
    uint64_t size;     // bytes
    uint64_t pages;
    uint64_t max_pages;
    bool has_max;
    bool is64;
    bool owned;        // this instance owns the reservation
} EaMemInst;

typedef struct EaTagInst {
    EaFuncType *type;
} EaTagInst;

typedef struct EaInstance {
    EaModule *module;
    uint32_t n_funcs; EaFuncInst *funcs;
    uint32_t n_tables; EaTableInst *tables;
    uint32_t n_memories; EaMemInst *memories;
    uint32_t n_globals; WVal *globals;
    uint32_t n_tags; EaTagInst *tags;
    uint32_t start_func; // absolute func index, UINT32_MAX if none
    // passive segment state
    uint8_t *data_alive;   // per data segment
    uint8_t *elem_alive;   // per elem segment
    struct EaStore *store;
    // JIT fast-path fields (maintained by the runtime)
    uint8_t *jit_mem_base;
    uint64_t jit_mem_limit;   // bytes
    WVal *jit_globals;
} EaInstance;

// ---------------------------------------------------------------- interpreter
// execution context (per-thread)
typedef struct EaExec {
    WVal *stack; uint32_t stack_cap, sp; // shared operand/locals stack across frames
    uint32_t depth;            // call depth
    uint32_t max_depth;
    EaTrap trap;               // pending trap code
    char trap_msg[96];         // optional detailed message
    void *jb;                  // jmp_buf* of the active entry (trap target)
} EaExec;

// interpreter entry: execute function whose args are already pushed on ex->stack.
// On success the params are replaced in place by the results.
// Returns 0 on success, 1 on trap.
int ea_interp_exec_function(EaExec *ex, EaFuncInst *fi);
int interp_ensure(EaExec *ex, uint32_t extra);
void ea_trap(EaExec *ex, EaTrap code);
bool ea_grow_memory(struct EaMemInst *mi, uint64_t delta, uint64_t *old);
int ea_jit_call(EaExec *ex, EaFuncInst *fi);
void ea_jit_compile_module(EaModule *m); // aarch64 baseline JIT (jit_a64.c)
void ea_instance_free(EaInstance *inst);

// host import bridge: host functions register a callback
typedef int (*EaHostFn)(void *user, const WVal *args, WVal *results);

// ---------------------------------------------------------------- public runtime API
typedef struct EaStore EaStore;

EaStore *ea_store_new(void);
void ea_store_set_max_depth(EaStore *s, uint32_t d);
const char *ea_store_last_trap_msg(EaStore *s);
void ea_store_free(EaStore *s);

// decode + validate. Returns 0 on success (err set on failure).
int ea_store_load(EaStore *s, const uint8_t *bytes, size_t len, EaModule **out, char **err);
// instantiate module; imports resolved by name among registered instances.
// Returns 0 on success; 1 = unlinkable; 2 = uninstantiable(trap); negative = internal.
int ea_store_instantiate(EaStore *s, EaModule *m, EaInstance **out, char **err_msg, EaTrap *trap);
void ea_register_instance(EaStore *s, const char *name, EaInstance *inst);
EaInstance *ea_lookup_instance(EaStore *s, const char *name);

int ea_instance_invoke(EaStore *s, EaInstance *inst, uint32_t func_idx,
                       const WVal *args, WVal *results, EaTrap *trap);
// find export index by name/kind; returns -1 when missing
int ea_instance_export(EaInstance *inst, const char *name, uint8_t kind);

#ifdef __cplusplus
}
#endif
#endif // EASM_H
