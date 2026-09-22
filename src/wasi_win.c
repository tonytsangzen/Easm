// Win32 backend (Windows) — UTF-8 paths converted to UTF-16, HANDLE I/O
#ifdef _WIN32
#include "wasi_platform.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int werr(DWORD e) {
    switch (e) {
    case ERROR_ACCESS_DENIED: return 2;
    case ERROR_FILE_NOT_FOUND: return 38;
    case ERROR_PATH_NOT_FOUND: return 38;
    case ERROR_ALREADY_EXISTS: return 12;
    case ERROR_FILE_EXISTS: return 12;
    case ERROR_INVALID_PARAMETER: return 21;
    case ERROR_NEGATIVE_SEEK: return 21;
    case ERROR_DISK_FULL: return 45;
    default: return 23;
    }
}

// UTF-8 -> UTF-16 (caller frees)
static wchar_t *utf16(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

// platform fd = HANDLE index in a per-process table kept by the caller is
// overkill: Win32 HANDLEs are small enough to store directly in our fd slots,
// so the "int fd" here is the HANDLE value truncated through an index table.
// For simplicity we keep a handle table.
#define WMAXH 256
static HANDLE g_h[WMAXH];

static int halloc(HANDLE h, int *pfd) {
    for (int i = 0; i < WMAXH; i++)
        if (!g_h[i]) { g_h[i] = h; *pfd = i; return 0; }
    CloseHandle(h);
    return 23;
}
static HANDLE hget(int fd) { return (fd >= 0 && fd < WMAXH) ? g_h[fd] : NULL; }
static void hfree(int fd) { if (fd >= 0 && fd < WMAXH) g_h[fd] = NULL; }

static int p_open(void *u, const char *path, int mode, int create, int trunc, int *pfd) {
    (void)u;
    wchar_t *w = utf16(path);
    DWORD access = (mode == 0) ? GENERIC_READ : (mode == 1 ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE);
    DWORD disp = OPEN_EXISTING;
    if (create && trunc) disp = CREATE_ALWAYS;
    else if (create) disp = OPEN_ALWAYS;
    else if (trunc) disp = TRUNCATE_EXISTING;
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return werr(GetLastError());
    return halloc(h, pfd);
}
static int p_close(void *u, int fd) { (void)u; if (!hget(fd)) return 8; CloseHandle(hget(fd)); hfree(fd); return 0; }
static int p_read(void *u, int fd, uint8_t *buf, size_t len, size_t *nread) {
    (void)u;
    HANDLE h = hget(fd); if (!h) return 8;
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)len, &got, NULL)) return werr(GetLastError());
    *nread = got;
    return 0;
}
static int p_write(void *u, int fd, const uint8_t *buf, size_t len, size_t *nw) {
    (void)u;
    HANDLE h = hget(fd); if (!h) return 8;
    DWORD wrote = 0;
    if (!WriteFile(h, buf, (DWORD)len, &wrote, NULL)) return werr(GetLastError());
    *nw = wrote;
    return 0;
}
static int p_seek(void *u, int fd, int64_t off, int whence, uint64_t *newofs) {
    (void)u;
    HANDLE h = hget(fd); if (!h) return 8;
    DWORD method = whence == 0 ? FILE_BEGIN : (whence == 1 ? FILE_CURRENT : FILE_END);
    LARGE_INTEGER li; li.QuadPart = off;
    LARGE_INTEGER np;
    if (!SetFilePointerEx(h, li, &np, method)) return werr(GetLastError());
    *newofs = (uint64_t)np.QuadPart;
    return 0;
}
static int p_sync(void *u, int fd) { (void)u; HANDLE h = hget(fd); if (!h) return 8; return FlushFileBuffers(h) ? 0 : werr(GetLastError()); }
static int p_fsize(void *u, int fd, uint64_t *size) {
    (void)u;
    HANDLE h = hget(fd); if (!h) return 8;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) return werr(GetLastError());
    *size = (uint64_t)li.QuadPart;
    return 0;
}
static int p_isdir_path(void *u, const char *path, int *isdir) {
    (void)u;
    wchar_t *w = utf16(path);
    DWORD a = GetFileAttributesW(w);
    free(w);
    if (a == INVALID_FILE_ATTRIBUTES) return werr(GetLastError());
    *isdir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return 0;
}
static int p_mkdir(void *u, const char *path) {
    (void)u;
    wchar_t *w = utf16(path);
    int r = CreateDirectoryW(w, NULL);
    free(w);
    return r ? 0 : werr(GetLastError());
}
static int p_unlink(void *u, const char *path) {
    (void)u;
    wchar_t *w = utf16(path);
    DWORD a = GetFileAttributesW(w);
    int r;
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
        r = RemoveDirectoryW(w);
    else
        r = DeleteFileW(w);
    free(w);
    return r ? 0 : werr(GetLastError());
}
// directory iteration model: pattern pointer + skip counter (demo surface;
// real integrators replace dirnext with a persistent FIND handle)
typedef struct { wchar_t *pat; int skip; } WDir;
static WDir g_dirs[WMAXH];

static int p_diropen(void *u, const char *path, int *ph) {
    (void)u;
    wchar_t *w = utf16(path);
    wchar_t *pat = (wchar_t *)malloc((wcslen(w) + 4) * sizeof(wchar_t));
    swprintf(pat, wcslen(w) + 4, L"%ls\\*", w);
    free(w);
    for (int i = 0; i < WMAXH; i++)
        if (!g_dirs[i].pat) { g_dirs[i].pat = pat; g_dirs[i].skip = 0; *ph = i; return 0; }
    free(pat);
    return 23;
}
static int p_dirclose(void *u, int h) {
    (void)u;
    if (h < 0 || h >= WMAXH || !g_dirs[h].pat) return 8;
    free(g_dirs[h].pat);
    g_dirs[h].pat = NULL;
    g_dirs[h].skip = 0;
    return 0;
}
static int p_dirnext(void *u, int h, char *name, size_t cap, int *isdir) {
    (void)u;
    if (h < 0 || h >= WMAXH || !g_dirs[h].pat) return 8;
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(g_dirs[h].pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return werr(GetLastError());
    BOOL ok = TRUE;
    for (int k = 0; k <= g_dirs[h].skip; k++) // skip . and previously returned
        ok = FindNextFileW(fh, &fd);
    FindClose(fh);
    if (!ok) { name[0] = 0; return 0; }
    g_dirs[h].skip++;
    snprintf(name, cap, "%ls", fd.cFileName);
    *isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return 0;
}
static int p_clock_ns(void *u, int id, uint64_t *out) {
    (void)u;
    if (id == 0) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER li; li.LowPart = ft.dwLowDateTime; li.HighPart = ft.dwHighDateTime;
        // 1601-01-01 -> 1970-01-01
        *out = (uint64_t)(li.QuadPart - 116444736000000000ull) * 100ull;
    } else {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        *out = (uint64_t)(c.QuadPart * 1000000000ull / f.QuadPart);
    }
    return 0;
}
static int p_random(void *u, uint8_t *buf, size_t len) {
    (void)u;
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(rand() & 0xff);
    return 0;
}
static void p_yield(void *u) { (void)u; SwitchToThread(); }
static int p_sleep_ns(void *u, uint64_t ns) { (void)u; Sleep((DWORD)(ns / 1000000ull)); return 0; }

// g_dir accessor shim (declared above)
static wchar_t **g_dir_tab(void);
static wchar_t *g_dir_w(int h) { return g_dir_tab()[(h & 0x3fff)]; }
static wchar_t **g_dir_tab(void) { static wchar_t *g_dir[WMAXH]; return g_dir; }

static const EaWasiPlat g_win = {
    p_open, p_close, p_read, p_write, p_seek, p_sync, p_fsize,
    p_isdir_path, p_mkdir, p_unlink,
    p_diropen, p_dirclose, p_dirnext,
    p_clock_ns, p_random, p_yield, p_sleep_ns,
};
const EaWasiPlat *ea_wasi_plat_win32(void) { return &g_win; }
#endif
