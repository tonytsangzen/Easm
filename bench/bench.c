// easm benchmark kernels (freestanding wasm32)
#define EXPORT __attribute__((export_name("fib")))
typedef unsigned int u32; typedef unsigned long long u64;

__attribute__((export_name("fib"))) u32 fib(u32 n) {
    if (n < 2) return n;
    u32 a = 0, b = 1;
    for (u32 i = 0; i < n; i++) { u32 t = a + b; a = b; b = t; }
    return a;
}

__attribute__((export_name("primes"))) u32 primes(u32 limit) {
    static unsigned char flags[1000000];
    u32 count = 0;
    for (u32 i = 0; i <= limit && i < 1000000; i++) flags[i] = 1;
    flags[0] = 0;
    if (limit > 0) flags[1] = 0;
    for (u32 i = 2; (u64)i * i <= limit && i < 1000000; i++)
        if (flags[i])
            for (u32 j = i * i; j <= limit && j < 1000000; j += i) flags[j] = 0;
    for (u32 i = 0; i <= limit && i < 1000000; i++) count += flags[i];
    return count;
}

__attribute__((export_name("sum")) ) u64 sum(u64 n) {
    u64 s = 0;
    for (u64 i = 1; i <= n; i++) s += i * 3 - (i >> 1);
    return s;
}

static float mat_a[128 * 128], mat_b[128 * 128], mat_c[128 * 128];

__attribute__((export_name("matmul"))) float matmul(u32 iters) {
    for (u32 i = 0; i < 128 * 128; i++) { mat_a[i] = (float)(i & 63); mat_b[i] = (float)((i >> 3) & 63); mat_c[i] = 0; }
    float acc = 0;
    for (u32 it = 0; it < iters; it++) {
        for (u32 i = 0; i < 128; i++) {
            for (u32 k = 0; k < 128; k++) {
                float a = mat_a[i * 128 + k];
                for (u32 j = 0; j < 128; j++)
                    mat_c[i * 128 + j] += a * mat_b[k * 128 + j];
            }
        }
        acc += mat_c[(it * 7) & 16383];
    }
    return acc;
}

__attribute__((export_name("memsum"))) u32 memsum(u32 n) {
    volatile u32 *p = (volatile u32 *)0x4000;
    for (u32 i = 0; i < 8192; i++) p[i] = i;
    u32 s = 0;
    for (u32 i = 0; i < n && i < 32768; i++) s += p[i & 8191];
    return s;
}

__attribute__((export_name("run"))) u32 run(u32 kind, u32 arg) {
    switch (kind) {
    case 0: return fib(arg);
    case 1: return primes(arg);
    case 2: return (u32)sum(arg);
    case 3: matmul(arg); return 0;
    default: return memsum(arg);
    }
}
