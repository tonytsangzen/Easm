// file demo: create + write + reopen + read back via a preopen directory
typedef unsigned int u32;

#define W(x) __attribute__((import_module("wasi_snapshot_preview1"), import_name(x)))

W("path_open") int path_open(int dirfd, int dirflags, const char *path, int plen,
                             int oflags, unsigned long long rights_base,
                             unsigned long long rights_inh, int fdflags, int *opened);
W("fd_write") int fd_write(int fd, const void *iovs, int iovs_len, int *nwritten);
W("fd_read") int fd_read(int fd, const void *iovs, int iovs_len, int *nread);
W("fd_close") int fd_close(int fd);
W("fd_seek") int fd_seek(int fd, long long off, int whence, int *newofs);

static int iov[2];
static void out(const char *s, int len) {
    iov[0] = (int)(long)s; iov[1] = len;
    int n; fd_write(1, iov, 1, &n);
}

int _start(void) {
    int fd = -1;
    // O_CREAT|O_TRUNC: 0x1|0x8 = 0x9
    int rc = path_open(3, 0, "demo.txt", 8, 0x9, ~0ull, ~0ull, 0, &fd);
    if (rc != 0) { out("open-for-write failed\n", 21); return 1; }
    const char *msg = "easm WASI file demo: hello from inside the sandbox\n";
    int len = 0;
    while (msg[len]) len++;
    int written = 0;
    iov[0] = (int)(long)msg; iov[1] = len;
    fd_write(fd, iov, 1, &written);
    fd_close(fd);
    {
        char nb[16]; int nn = 0, v = written;
        do { nb[nn++] = '0' + v % 10; v /= 10; } while (v);
        const char *wt = "written=";
        for (int k = 0; k < 8; k++) out(wt + k, 1);
        for (int k = nn - 1; k >= 0; k--) out(nb + k, 1);
        out("\n", 1);
    }

    out("round-trip: ", 12);
    rc = path_open(3, 0, "demo.txt", 8, 0, ~0ull, ~0ull, 0, &fd);
    if (rc != 0) { out("reopen failed\n", 14); return 1; }
    static char rdbuf[128];
    iov[0] = (int)(long)rdbuf; iov[1] = 128;
    int nread = 0;
    fd_read(fd, iov, 1, &nread);
    fd_close(fd);
    out(rdbuf, nread);
    // verify content
    int ok = nread == len;
    for (int i = 0; i < len && i < nread; i++) if (rdbuf[i] != msg[i]) ok = 0;
    out("nread=", 6);
    {
        char nb[16];
        int nn = 0, v = nread;
        do { nb[nn++] = '0' + v % 10; v /= 10; } while (v);
        for (int k = nn - 1; k >= 0; k--) out(nb + k, 1);
    }
    out(" len=", 5);
    {
        char nb[16];
        int nn = 0, v = len;
        do { nb[nn++] = '0' + v % 10; v /= 10; } while (v);
        for (int k = nn - 1; k >= 0; k--) out(nb + k, 1);
    }
    out("\n", 1);
    out(rdbuf, nread);
    out("\n", 1);
    out(ok ? "verify: OK\n" : "verify: MISMATCH\n", ok ? 11 : 17);
    return ok ? 0 : 1;
}
