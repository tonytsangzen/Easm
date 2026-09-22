// CLI demo: banner + clock + argv echo, via WASI preview 1
typedef unsigned int u32;
typedef unsigned long long u64;
static unsigned int my_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

#define W(x) __attribute__((import_module("wasi_snapshot_preview1"), import_name(x)))

W("fd_write") int fd_write(int fd, const void *iovs, int iovs_len, int *nwritten);
W("args_sizes_get") int args_sizes_get(int *count, int *bufsize);
W("args_get") int args_get(char **argv, char *buf);
W("clock_time_get") int clock_time_get(int id, unsigned long long prec, unsigned long long *t);

static int iov[2]; // ptr, len pairs

static void out(const char *s, int len) {
    iov[0] = (int)(long)s;
    iov[1] = len;
    int n;
    fd_write(1, iov, 1, &n);
}

static void utoa(char *dst, unsigned long long v) {
    char tmp[24];
    int n = 0;
    do { tmp[n++] = '0' + (char)(v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
    dst[n] = 0;
}

int _start(void) {
    static char line[512];
    int p = 0;
    const char *hdr = "== easm WASI echo demo ==\n";
    int hl = 26;
    for (int i = 0; i < hl; i++) line[p++] = hdr[i];

    unsigned long long t = 0;
    clock_time_get(1, 1, &t); // monotonic
    const char *ck = "clock: ";
    for (int i = 0; i < 7; i++) line[p++] = ck[i];
    utoa(line + p, t); (void)0;
    p += my_len(line + p);
    line[p++] = '\n';

    int count = 0, bufsize = 0;
    args_sizes_get(&count, &bufsize);
    static char *argv[32];
    static char buf[1024];
    args_get(argv, buf);
    const char *ap = "argc: ";
    for (int i = 0; i < 6; i++) line[p++] = ap[i];
    utoa(line + p, (unsigned long long)count);
    p += my_len(line + p);
    line[p++] = '\n';

    out(line, p);
    p = 0;
    for (int i = 1; i < count; i++) {
        const char *pre = "arg: ";
        for (int k = 0; k < 5; k++) line[p++] = pre[k];
        const char *a = argv[i];
        while (*a) line[p++] = *a++;
        line[p++] = '\n';
        out(line, p);
        p = 0;
    }
    const char *bye = "== done ==\n";
    for (int i = 0; i < 10; i++) line[p++] = bye[i];
    out(line, p);
    return 0;
}
