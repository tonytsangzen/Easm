// easm GUI viewer — AppKit window displaying a WASI-computed framebuffer
#import <AppKit/AppKit.h>
#import <stdlib.h>
#import <string.h>

#include "easm.h"
#include "wasi.h"

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) {
            printf("usage: easm_gui <module.wasm>\n");
            return 2;
        }
        const char *mod_path = argv[1];
        __block WasiCtx *ctx = ea_wasi_ctx_new(NULL, 0, NULL, 0);
        EaStore *store = ea_store_new();
        char *err = NULL;
        ea_wasi_init(store, ctx, &err);
        ea_wasi_framebuffer(ctx, "efb", 4 + 320 * 240 * 4 + 4096); // hdr + max frame

        FILE *f = fopen(mod_path, "rb");
        if (!f) { printf("open failed\n"); return 2; }
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(len);
        fread(buf, 1, len, f); fclose(f);
        EaModule *m = NULL;
        if (ea_store_load(store, buf, len, &m, &err) != 0) {
            printf("load: %s\n", err ?: "?"); return 1;
        }
        EaInstance *inst = NULL;
        EaTrap trap;
        if (ea_store_instantiate(store, m, &inst, &err, &trap) != 0) {
            printf("inst: %s\n", err ?: ea_trap_msg(trap)); return 1;
        }
        ea_wasi_bind(ctx, inst);
        int idx = ea_instance_export(inst, "_start", EAK_FUNC);
        if (idx < 0) { printf("no _start\n"); return 2; }
        ea_instance_invoke(store, inst, idx, NULL, NULL, &trap);

        // parse the framebuffer: [FB magic][u32 w][u32 h][RGBA pixels...]
        // find the framebuffer fd
        uint8_t *pixels = NULL;
        uint32_t wid = 0, hei = 0;
        for (uint32_t i = 3; i < ctx->cap_fds; i++) {
            if (ctx->fds[i].used && ctx->fds[i].fb && ctx->fds[i].fb_len > 12) {
                uint8_t *fb = ctx->fds[i].fb;
                if (fb[0] == 'F' && fb[1] == 'B') {
                    wid = (fb[2] << 24) | (fb[3] << 16) | (fb[4] << 8) | fb[5];
                    hei = (fb[6] << 24) | (fb[7] << 16) | (fb[8] << 8) | fb[9];
                    pixels = fb + 12;
                    printf("framebuffer: %ux%u (%u bytes data)\n", wid, hei, ctx->fds[i].fb_len - 12);
                    break;
                }
            }
        }
        if (!pixels || !wid || !hei) { printf("no framebuffer output\n"); return 1; }

        // create NSImage from raw RGBA
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL pixelsWide:wid pixelsHigh:hei
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
            colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:wid*4 bitsPerPixel:32
            ];
        uint8_t *dst = [rep bitmapData];
        memcpy(dst, pixels, (size_t)wid * hei * 4);
        // BGRA → RGBA swap for NSBitmapImageRep (which expects BGRA in device RGB)
        for (uint32_t k = 0; k < (uint32_t)wid * hei; k++) {
            uint8_t t = dst[k*4+0]; dst[k*4+0] = dst[k*4+2]; dst[k*4+2] = t;
        }
        NSImage *img = [[NSImage alloc] init];
        [img addRepresentation:rep];

        // window
        NSRect wr = NSMakeRect(100, 100, wid * 2, hei * 2);
        NSWindow *win = [[NSWindow alloc]
            initWithContentRect:wr styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        [win setTitle:@"easm WASI GUI demo — Mandelbrot"];
        NSImageView *iv = [[NSImageView alloc] initWithFrame:win.contentView.bounds];
        iv.image = img;
        iv.imageScaling = NSImageScaleProportionallyUpOrDown;
        [win.contentView addSubview:iv];
        [win center];
        [win makeKeyAndOrderFront:nil];
        [win makeMainWindow];

        // run the AppKit event loop (modal until window closes)
        while (win.isVisible) {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                untilDate:[NSDate distantFuture] inMode:NSDefaultRunLoopMode dequeue:YES];
            if (e) [NSApp sendEvent:e];
        }
        return 0;
    }
}
