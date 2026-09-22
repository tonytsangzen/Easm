// GUI demo: Mandelbrot/gradient written to the WASI framebuffer device
typedef unsigned int u32;
typedef unsigned char u8;

#define W(x) __attribute__((import_module("wasi_snapshot_preview1"), import_name(x)))
W("fd_write") int fd_write(int fd, const void *iovs, int iovs_len, int *nwritten);

#define WID 320
#define HEI 240

static u8 frame[WID * HEI * 4];

int _start(void) {
    // write resolution header: 4 bytes magic 'FB', then u32 w, u32 h
    u8 hdr[12] = {'F', 'B', (u8)(WID >> 24), (u8)(WID >> 16), (u8)(WID >> 8), (u8)WID,
                  (u8)(HEI >> 24), (u8)(HEI >> 16), (u8)(HEI >> 8), (u8)HEI, 0, 4};
    int iov[2] = {(int)(long)hdr, 12};
    int nw = 0;
    fd_write(3, iov, 1, &nw);

    for (u32 y = 0; y < HEI; y++) {
        for (u32 x = 0; x < WID; x++) {
            // Mandelbrot
            float cx = (float)x / WID * 3.5f - 2.5f;
            float cy = (float)y / HEI * 2.0f - 1.0f;
            float zx = 0, zy = 0;
            int iter = 0;
            while (zx * zx + zy * zy < 4.0f && iter < 64) {
                float nz = zx * zx - zy * zy + cx;
                zy = 2.0f * zx * zy + cy;
                zx = nz;
                iter++;
            }
            u32 o = (y * WID + x) * 4;
            frame[o + 0] = (u8)(iter * 4);
            frame[o + 1] = (u8)(iter * 2);
            frame[o + 2] = (u8)(iter < 64 ? iter * 3 : 255);
            frame[o + 3] = 255;
        }
    }
    // write the entire frame as one iovec
    iov[0] = (int)(long)frame;
    iov[1] = WID * HEI * 4;
    fd_write(3, iov, 1, &nw);
    return (nw == WID * HEI * 4) ? 0 : 1;
}
