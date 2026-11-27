#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/mux_types.h>
#include "tv.h"
#include "image_decode.h"

static void dims(int w, int h, int *ow, int *oh) {
    double s = 1.;
    if (w > 320) s = 320. / w;
    if (h * s > 180) s = 180. / h;
    *ow = w * s; *oh = h * s;
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}
static int rgb_out(const unsigned char *rgb, int w, int h, int stride, kimg_t *out) {
    int ow, oh, x, y; dims(w, h, &ow, &oh);
    u32 *px = malloc(ow * oh * sizeof(u32));
    if (!px) return 0;
    for (y = 0; y < oh; y++) for (x = 0; x < ow; x++) {
        const unsigned char *p = rgb + (y * h / oh) * stride + (x * w / ow) * 3;
        px[y * ow + x] = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
    }
    out->w = ow; out->h = oh; out->px = px; return 1;
}
/* transparencia (emotes, avatares) sobre el fondo de paneles en vez de negro. */
static int rgba_out(const unsigned char *rgba, int w, int h, int stride, kimg_t *out) {
    int ow, oh, x, y; dims(w, h, &ow, &oh);
    u32 *px = malloc(ow * oh * sizeof(u32));
    if (!px) return 0;
    const int br = (C_BG2 >> 16) & 255, bgc = (C_BG2 >> 8) & 255, bb = C_BG2 & 255;
    for (y = 0; y < oh; y++) for (x = 0; x < ow; x++) {
        const unsigned char *p = rgba + (y * h / oh) * stride + (x * w / ow) * 4;
        int a = p[3];
        int r = (p[0] * a + br * (255 - a)) / 255, g = (p[1] * a + bgc * (255 - a)) / 255, b = (p[2] * a + bb * (255 - a)) / 255;
        px[y * ow + x] = ((u32)r << 16) | ((u32)g << 8) | b;
    }
    out->w = ow; out->h = oh; out->px = px; return 1;
}
typedef struct { struct jpeg_error_mgr pub; jmp_buf jump; unsigned char *rgb; } jpeg_err;
static void jpeg_fail(j_common_ptr c) { longjmp(((jpeg_err *)c->err)->jump, 1); }
static int from_jpeg(const unsigned char *data, unsigned size, kimg_t *out) {
    struct jpeg_decompress_struct c; jpeg_err e;
    memset(&c, 0, sizeof(c)); memset(&e, 0, sizeof(e));
    c.err = jpeg_std_error(&e.pub); e.pub.error_exit = jpeg_fail;
    if (setjmp(e.jump)) { free(e.rgb); jpeg_destroy_decompress(&c); return 0; }
    jpeg_create_decompress(&c); jpeg_mem_src(&c, data, size);
    jpeg_read_header(&c, TRUE);
    if (!c.image_width || !c.image_height || c.image_width > 8192 || c.image_height > 8192) { jpeg_destroy_decompress(&c); return 0; }
    c.out_color_space = JCS_RGB;
    /* escala DCT hasta la resolucion que se muestra (max 320x180):
       un banner de 2560x1440 se decodifica directo a 320x180 (1/8). */
    { int tw, th; dims(c.image_width, c.image_height, &tw, &th);
      c.scale_num = 1; c.scale_denom = 1;
      while (c.scale_denom < 8 && c.image_width / (c.scale_denom * 2) >= (unsigned)tw && c.image_height / (c.scale_denom * 2) >= (unsigned)th) c.scale_denom *= 2; }
    c.dct_method = JDCT_IFAST; c.do_fancy_upsampling = FALSE; c.do_block_smoothing = FALSE;
    jpeg_calc_output_dimensions(&c);
    if ((unsigned long)c.output_width * c.output_height > 1600UL * 1024) { jpeg_destroy_decompress(&c); return 0; }
    jpeg_start_decompress(&c);
    int w = c.output_width, h = c.output_height, stride = w * 3;
    e.rgb = malloc(stride * h);
    if (!e.rgb) { jpeg_destroy_decompress(&c); return 0; }
    while (c.output_scanline < c.output_height) {
        JSAMPROW row = e.rgb + c.output_scanline * stride;
        jpeg_read_scanlines(&c, &row, 1);
    }
    jpeg_finish_decompress(&c);
    int ok = rgb_out(e.rgb, w, h, stride, out);
    free(e.rgb); jpeg_destroy_decompress(&c); return ok;
}
typedef struct {
    const unsigned char *data; unsigned size, pos;
    unsigned char *row, *full; u32 *pixels;
} png_state;
static void png_memread(png_structp p, png_bytep dst, png_size_t n) {
    png_state *st=png_get_io_ptr(p);
    if(n>st->size-st->pos) png_error(p,"truncated image");
    memcpy(dst,st->data+st->pos,n);st->pos+=n;
}
static int from_png(const unsigned char *data,unsigned size,kimg_t *out) {
    png_state *st=calloc(1,sizeof(*st)); if(!st)return 0;
    st->data=data;st->size=size;
    png_structp p=png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop info=p?png_create_info_struct(p):NULL;
    if(!p||!info) { if(p)png_destroy_read_struct(&p,NULL,NULL);free(st);return 0; }
    if(setjmp(png_jmpbuf(p))) { free(st->row);free(st->full);free(st->pixels);png_destroy_read_struct(&p,&info,NULL);free(st);return 0; }
    png_set_read_fn(p,st,png_memread);png_set_crc_action(p,PNG_CRC_QUIET_USE,PNG_CRC_QUIET_USE);png_read_info(p,info);
    int w=png_get_image_width(p,info),h=png_get_image_height(p,info);
    if(w<1||h<1||w>8192||h>8192||(unsigned)w*h>16*1024*1024) png_error(p,"image too large");
    int type=png_get_color_type(p,info),depth=png_get_bit_depth(p,info);
    if(depth==16)png_set_strip_16(p);
    if(type==PNG_COLOR_TYPE_PALETTE)png_set_palette_to_rgb(p);
    if(type==PNG_COLOR_TYPE_GRAY&&depth<8)png_set_expand_gray_1_2_4_to_8(p);
    if(png_get_valid(p,info,PNG_INFO_tRNS))png_set_tRNS_to_alpha(p);
    if(type==PNG_COLOR_TYPE_GRAY||type==PNG_COLOR_TYPE_GRAY_ALPHA)png_set_gray_to_rgb(p);
    if((type&PNG_COLOR_MASK_ALPHA)||png_get_valid(p,info,PNG_INFO_tRNS)) {
        png_color_16 bgc; memset(&bgc,0,sizeof(bgc));
        bgc.red=(C_BG2>>16)&255;bgc.green=(C_BG2>>8)&255;bgc.blue=C_BG2&255;
        png_set_background(p,&bgc,PNG_BACKGROUND_GAMMA_SCREEN,0,1.0);
    }
    png_set_strip_alpha(p);
    int passes=png_set_interlace_handling(p);png_read_update_info(p,info);
    int stride=png_get_rowbytes(p,info),ow,oh,x,y,j,pass;
    if(stride!=w*3)png_error(p,"unexpected pixel format");
    dims(w,h,&ow,&oh);st->pixels=malloc(ow*oh*4);st->row=malloc(stride);
    if(!st->pixels||!st->row)png_error(p,"allocation failed");
    if(passes>1) {
        if((unsigned)w*h>1280*1024)png_error(p,"interlaced image too large");
        st->full=calloc(h,stride);if(!st->full)png_error(p,"allocation failed");
    }
    for(pass=0;pass<passes;pass++) {
        j=0;
        for(y=0;y<h;y++) {
            unsigned char *row=st->full?st->full+y*stride:st->row;
            png_read_row(p,row,NULL);
            if(j<oh&&y==j*h/oh) {
                for(x=0;x<ow;x++) { unsigned char *q=row+x*w/ow*3;st->pixels[j*ow+x]=((u32)q[0]<<16)|((u32)q[1]<<8)|q[2]; }
                j++;
            }
        }
    }
    png_read_end(p,NULL);out->w=ow;out->h=oh;out->px=st->pixels;
    free(st->row);free(st->full);png_destroy_read_struct(&p,&info,NULL);free(st);return 1;
}

/* Cuadro WebP suelto (VP8/VP8L sin RIFF, tal como lo da el demuxer). */
static int webp_plain(const uint8_t *d, size_t n, kimg_t *out) {
    int w = 0, h = 0;
    uint8_t *rgb = NULL;
    if (!WebPGetInfo(d, n, &w, &h) || w < 1 || h < 1 || w > 2048 || h > 2048) return 0;
    rgb = WebPDecodeRGBA(d, n, &w, &h);
    if (!rgb) return 0;
    int ok = rgba_out(rgb, w, h, w * 4, out);
    WebPFree(rgb); return ok;
}
/* GIF completo (todos los cuadros, disposal 0-3, paletas locales, transparencia,
 * entrelazado). Recorre los cuadros sobre un lienzo ARGB (A=0 transparente) y llama a cb
 * por cada uno. LZW acotado; el lienzo se limita a 512x512 (emotes). */
typedef int (*frame_cb)(void *ctx, const u32 *cv, int w, int h, int delay_ms, int index);
static int lzw_decode(const unsigned char *lz, unsigned lzn, int mcs, unsigned char *idx, int total,
                      unsigned short *prefix, unsigned char *suffix, unsigned char *stack) {
    int clear = 1 << mcs, eoi = clear + 1, avail = clear + 2, csize = mcs + 1, cmask = (1 << csize) - 1, old = -1, first = 0, i;
    unsigned bits = 0, acc = 0, bp = 0; int px = 0;
    for (i = 0; i < clear; i++) { prefix[i] = 0; suffix[i] = i; }
    while (px < total) {
        while (bits < (unsigned)csize && bp < lzn) { acc |= (unsigned)lz[bp++] << bits; bits += 8; }
        if (bits < (unsigned)csize) break;
        int code = acc & cmask; acc >>= csize; bits -= csize;
        if (code == clear) { csize = mcs + 1; cmask = (1 << csize) - 1; avail = clear + 2; old = -1; continue; }
        if (code == eoi) break;
        if (old < 0) { if (code >= clear) break; idx[px++] = suffix[code]; old = code; first = code; continue; }
        int in = code, sp = 0;
        if (code > avail) break;
        if (code == avail) { stack[sp++] = first; code = old; }
        while (code >= clear && sp < 4096) { stack[sp++] = suffix[code]; code = prefix[code]; }
        first = suffix[code]; stack[sp++] = first;
        if (avail < 4096) { prefix[avail] = old; suffix[avail] = first; avail++; if (!(avail & cmask) && avail < 4096) { csize++; cmask = (1 << csize) - 1; } }
        old = in;
        while (sp && px < total) idx[px++] = stack[--sp];
    }
    return px;
}
/* cuenta cuadros sin decodificar */
static int gif_count(const unsigned char *d, unsigned n) {
    unsigned pos = 13; int c = 0;
    if (n < 13 || memcmp(d, "GIF8", 4)) return 0;
    if (d[10] & 0x80) pos += 3u << ((d[10] & 7) + 1);
    while (pos < n) {
        if (d[pos] == 0x21) { pos += 2; while (pos < n && d[pos]) pos += d[pos] + 1; pos++; }
        else if (d[pos] == 0x2C) {
            if (pos + 10 > n) break;
            int lf = d[pos + 9]; pos += 10;
            if (lf & 0x80) pos += 3u << ((lf & 7) + 1);
            pos++;
            while (pos < n && d[pos]) pos += d[pos] + 1;
            pos++; c++;
        } else break;
    }
    return c;
}
static int gif_iterate(const unsigned char *d, unsigned n, frame_cb cb, void *ctx, int maxframes) {
    unsigned pos = 13;
    if (n < 13 || memcmp(d, "GIF8", 4)) return 0;
    int sw = d[6] | (d[7] << 8), sh = d[8] | (d[9] << 8), gflags = d[10];
    unsigned char gpal[768], lpal[768];
    int trans = -1, disp = 0, delay = 0, frames = 0, stop = 0;
    memset(gpal, 0, sizeof(gpal));
    if (sw < 1 || sh < 1 || sw > 512 || sh > 512) return 0;
    if (gflags & 0x80) { unsigned gs = 3u << ((gflags & 7) + 1); if (pos + gs > n) return 0; memcpy(gpal, d + pos, gs); pos += gs; }
    u32 *cv = calloc((size_t)sw * sh, 4), *prev = NULL;
    unsigned char *lz = malloc(n), *idx = malloc((size_t)sw * sh);
    unsigned short *prefix = malloc(4096 * 2); unsigned char *suffix = malloc(4096), *stack = malloc(4097);
    if (!cv || !lz || !idx || !prefix || !suffix || !stack) goto done;
    while (pos < n && !stop) {
        if (d[pos] == 0x21) {
            if (pos + 2 > n) break;
            if (d[pos + 1] == 0xF9 && pos + 7 < n && d[pos + 2] == 4) {
                int pk = d[pos + 3];
                disp = (pk >> 2) & 7; delay = (d[pos + 4] | (d[pos + 5] << 8)) * 10;
                trans = (pk & 1) ? d[pos + 6] : -1;
            }
            pos += 2;
            while (pos < n && d[pos]) pos += d[pos] + 1;
            pos++;
        } else if (d[pos] == 0x2C) {
            if (pos + 10 > n) break;
            int ix = d[pos + 1] | (d[pos + 2] << 8), iy = d[pos + 3] | (d[pos + 4] << 8);
            int iw = d[pos + 5] | (d[pos + 6] << 8), ih = d[pos + 7] | (d[pos + 8] << 8), lf = d[pos + 9];
            const unsigned char *pal = gpal;
            pos += 10;
            if (lf & 0x80) { unsigned ls = 3u << ((lf & 7) + 1); if (pos + ls > n) break; memset(lpal, 0, sizeof lpal); memcpy(lpal, d + pos, ls); pos += ls; pal = lpal; }
            if (pos >= n) break;
            int mcs = d[pos++];
            unsigned lzn = 0;
            while (pos < n && d[pos]) { unsigned b = d[pos++]; if (pos + b > n) b = n - pos; memcpy(lz + lzn, d + pos, b); lzn += b; pos += b; }
            pos++;
            if (mcs < 2 || mcs > 8 || iw < 1 || ih < 1) break;
            /* recortar el rect al lienzo */
            if (ix >= sw || iy >= sh) { trans = -1; disp = 0; delay = 0; continue; }
            int total = iw * ih;
            if ((size_t)total > (size_t)sw * sh) { /* cuadro mas grande que el lienzo: sólo lo que entra */
                if (iw > sw || ih > sh) break;
            }
            memset(idx, trans >= 0 ? trans : 0, total);
            lzw_decode(lz, lzn, mcs, idx, total, prefix, suffix, stack);
            if (disp == 3) { if (!prev) prev = malloc((size_t)sw * sh * 4); if (prev) memcpy(prev, cv, (size_t)sw * sh * 4); }
            int inter = lf & 0x40, row = 0, pass = 0, x, y;
            static const int st[4] = { 0, 4, 2, 1 }, inc[4] = { 8, 8, 4, 2 };
            for (y = 0; y < ih; y++) {
                int ry = y;
                if (inter) { ry = row; row += inc[pass]; while (row >= ih && pass < 3) { pass++; row = st[pass]; } }
                int dy = iy + ry;
                if (dy >= sh) continue;
                for (x = 0; x < iw; x++) {
                    int c = idx[y * iw + x], dx = ix + x;
                    if (c == trans || dx >= sw) continue;
                    cv[dy * sw + dx] = 0xFF000000u | ((u32)pal[c * 3] << 16) | ((u32)pal[c * 3 + 1] << 8) | pal[c * 3 + 2];
                }
            }
            if (cb(ctx, cv, sw, sh, delay, frames)) stop = 1;
            frames++;
            if (maxframes && frames >= maxframes) stop = 1;
            if (disp == 2) {
                for (y = iy; y < iy + ih && y < sh; y++) for (x = ix; x < ix + iw && x < sw; x++) cv[y * sw + x] = 0;
            } else if (disp == 3 && prev) memcpy(cv, prev, (size_t)sw * sh * 4);
            trans = -1; disp = 0; delay = 0;
        } else break;
    }
done:
    free(cv); free(prev); free(lz); free(idx); free(prefix); free(suffix); free(stack);
    return frames;
}
/* primer cuadro como imagen estatica (lo transparente queda con el fondo del chat) */
static int argb_bg_out(const u32 *cv, int w, int h, kimg_t *out) {
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return 0;
    const int br = (C_BG2 >> 16) & 255, bgc = (C_BG2 >> 8) & 255, bb = C_BG2 & 255;
    for (int i = 0; i < w * h; i++) {
        u32 c = cv[i]; int a = c >> 24;
        unsigned char *q = rgb + i * 3;
        q[0] = (((c >> 16) & 255) * a + br * (255 - a)) / 255;
        q[1] = (((c >> 8) & 255) * a + bgc * (255 - a)) / 255;
        q[2] = ((c & 255) * a + bb * (255 - a)) / 255;
    }
    int ok = rgb_out(rgb, w, h, w * 3, out);
    free(rgb); return ok;
}
static int first_cb(void *ctx, const u32 *cv, int w, int h, int dl, int i) {
    (void)dl; (void)i; *(int *)((void **)ctx)[1] = argb_bg_out(cv, w, h, (kimg_t *)((void **)ctx)[0]); return 1;
}
static int from_gif(const unsigned char *d, unsigned n, kimg_t *out) {
    int ok = 0; void *ctx[2] = { out, &ok };
    gif_iterate(d, n, first_cb, ctx, 1);
    return ok;
}

/* ---- animacion reducida a la altura del chat (RGB565 + color clave) ---- */
typedef struct { kanim_t *a; int step, keep, src, th, tw, maxf, acc; } abuild_t;
static int anim_cb(void *ctx, const u32 *cv, int sw, int sh, int dl, int i) {
    abuild_t *b = ctx; kanim_t *a = b->a;
    (void)i;
    if (dl < 20) dl = 100;            /* como los navegadores */
    b->acc += dl;
    if ((b->src++ % b->step) != 0 && a->n > 0) { a->dly[a->n - 1] += dl; b->acc = 0; return 0; }
    if (a->n >= b->maxf) { a->dly[a->n - 1] += dl; b->acc = 0; return 0; }
    u16 *o = a->px + (size_t)a->n * a->w * a->h;
    int x, y;
    for (y = 0; y < a->h; y++) {
        int y0 = y * sh / a->h, y1 = (y + 1) * sh / a->h; if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < a->w; x++) {
            int x0 = x * sw / a->w, x1 = (x + 1) * sw / a->w; if (x1 <= x0) x1 = x0 + 1;
            unsigned sa = 0, sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
                u32 c = cv[yy * sw + xx]; unsigned al = c >> 24;
                sa += al; sr += ((c >> 16) & 255) * al; sg += ((c >> 8) & 255) * al; sb += (c & 255) * al; cnt++;
            }
            u16 v = ANIM_KEY;
            if (sa * 2 >= cnt * 255 && sa) {
                unsigned r = sr / sa, g = sg / sa, bl = sb / sa;
                v = ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3);
                if (v == ANIM_KEY) v ^= 0x20;
            }
            o[y * a->w + x] = v;
        }
    }
    a->dly[a->n++] = dl > 65000 ? 65000 : dl;
    b->acc = 0;
    return 0;
}
static kanim_t *anim_alloc(int sw, int sh, int nsrc, int th, int maxw, int maxbytes, int maxf, abuild_t *b) {
    if (nsrc < 2 || sw < 1 || sh < 1) return NULL;
    int tw = (sw * th + sh / 2) / sh; if (tw < 1) tw = 1; if (tw > maxw) tw = maxw;
    int fb = tw * th * 2;
    int mf = maxbytes / fb; if (mf > maxf) mf = maxf;
    if (mf < 2) return NULL;
    int step = (nsrc + mf - 1) / mf; if (step < 1) step = 1;
    kanim_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    int keep = (nsrc + step - 1) / step; if (keep > mf) keep = mf;
    a->w = tw; a->h = th;
    a->px = malloc((size_t)keep * fb); a->dly = malloc(keep * 2);
    if (!a->px || !a->dly) { free(a->px); free(a->dly); free(a); return NULL; }
    memset(b, 0, sizeof *b);
    b->a = a; b->step = step; b->maxf = keep; b->th = th; b->tw = tw;
    return a;
}
static void anim_finish(kanim_t *a) {
    int i; a->total = 0;
    for (i = 0; i < a->n; i++) a->total += a->dly[i];
    a->bytes = sizeof *a + (u32)a->n * a->w * a->h * 2 + a->n * 2;
}
void anim_free(kanim_t *a) { if (a) { free(a->px); free(a->dly); free(a); } }
kanim_t *image_anim(const unsigned char *d, unsigned n, int th, int maxw, int maxbytes, int maxf) {
    abuild_t b; kanim_t *a = NULL;
    if (n < 16) return NULL;
    if (!memcmp(d, "GIF8", 4)) {
        int sw = d[6] | (d[7] << 8), sh = d[8] | (d[9] << 8);
        if (sw > 256 || sh > 256) return NULL;   /* lienzo + copia: <= 512 KB transitorios */
        int nf = gif_count(d, n);
        a = anim_alloc(sw, sh, nf, th, maxw, maxbytes, maxf, &b);
        if (!a) return NULL;
        gif_iterate(d, n, anim_cb, &b, 0);
    } else if (!memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {
        WebPAnimDecoderOptions op; WebPData wd = { d, n };
        if (!WebPAnimDecoderOptionsInit(&op)) return NULL;
        op.color_mode = MODE_RGBA; op.use_threads = 0;
        WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &op);
        WebPAnimInfo inf;
        if (!dec) return NULL;
        if (!WebPAnimDecoderGetInfo(dec, &inf) || inf.canvas_width > 256 || inf.canvas_height > 256 ||
            !(a = anim_alloc(inf.canvas_width, inf.canvas_height, inf.frame_count, th, maxw, maxbytes, maxf, &b))) { WebPAnimDecoderDelete(dec); return NULL; }
        u32 *cv = malloc((size_t)inf.canvas_width * inf.canvas_height * 4);
        int prevts = 0, i = 0;
        while (cv && WebPAnimDecoderHasMoreFrames(dec)) {
            uint8_t *buf; int ts;
            if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) break;
            for (unsigned k = 0; k < inf.canvas_width * inf.canvas_height; k++) {
                const uint8_t *p = buf + k * 4;
                cv[k] = ((u32)p[3] << 24) | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            }
            anim_cb(&b, cv, inf.canvas_width, inf.canvas_height, ts - prevts, i++);
            prevts = ts;
        }
        free(cv);
        WebPAnimDecoderDelete(dec);
    } else return NULL;
    if (a->n < 2) { anim_free(a); return NULL; }
    anim_finish(a);
    return a;
}
int image_decode(const unsigned char *data, unsigned size, kimg_t *out) {
    if (size < 12) return 0;
    if (data[0] == 0xff && data[1] == 0xd8) return from_jpeg(data, size, out);
    if (!png_sig_cmp(data, 0, 8)) return from_png(data,size,out);
    if (!memcmp(data, "GIF8", 4)) return from_gif(data, size, out);
    if (!memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) {
        WebPDecoderConfig cfg;
        if (!WebPInitDecoderConfig(&cfg) || WebPGetFeatures(data, size, &cfg.input) != VP8_STATUS_OK) return 0;
        if (cfg.input.width > 8192 || cfg.input.height > 8192) return 0;
        if (cfg.input.has_animation) {   /* emotes animados -> primer cuadro */
            WebPData wd = { data, size }; int ok = 0;
            WebPDemuxer *dm = WebPDemux(&wd); WebPIterator it;
            if (dm && WebPDemuxGetFrame(dm, 1, &it)) {
                ok = webp_plain(it.fragment.bytes, it.fragment.size, out);
                WebPDemuxReleaseIterator(&it);
            }
            WebPDemuxDelete(dm); return ok;
        }
        int w, h; dims(cfg.input.width, cfg.input.height, &w, &h);
        cfg.options.use_scaling = 1; cfg.options.scaled_width = w; cfg.options.scaled_height = h;
        int alpha = cfg.input.has_alpha;
        cfg.output.colorspace = alpha ? MODE_RGBA : MODE_RGB;
        int ok = WebPDecode(data, size, &cfg) == VP8_STATUS_OK;
        if (ok) ok = alpha ? rgba_out(cfg.output.u.RGBA.rgba, w, h, cfg.output.u.RGBA.stride, out)
                           : rgb_out(cfg.output.u.RGBA.rgba, w, h, cfg.output.u.RGBA.stride, out);
        WebPFreeDecBuffer(&cfg.output); return ok;
    }
    return 0;
}
