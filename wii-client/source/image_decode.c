#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/demux.h>
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
/* rc13: transparencia (emotes, avatares) sobre el fondo de paneles en vez de negro. */
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
    /* rc13: escala DCT hasta la resolucion que se muestra (max 320x180):
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
/* GIF: solo el primer cuadro (emotes animados de Kick). LZW acotado. */
static int from_gif(const unsigned char *d, unsigned n, kimg_t *out) {
    unsigned pos = 13, i;
    if (n < 13) return 0;
    int sw = d[6] | (d[7] << 8), sh = d[8] | (d[9] << 8), gflags = d[10], bg = d[11];
    unsigned char gpal[768], lpal[768], *pal = gpal; int trans = -1;
    memset(gpal, 0, sizeof(gpal));
    if (sw < 1 || sh < 1 || sw > 1024 || sh > 1024) return 0;
    if (gflags & 0x80) { unsigned gs = 3u << ((gflags & 7) + 1); if (pos + gs > n) return 0; memcpy(gpal, d + pos, gs); pos += gs; }
    while (pos < n) {
        if (d[pos] == 0x21) {   /* extension */
            if (pos + 2 > n) return 0;
            if (d[pos + 1] == 0xF9 && pos + 7 < n && d[pos + 2] == 4 && (d[pos + 3] & 1)) trans = d[pos + 6];
            pos += 2;
            while (pos < n && d[pos]) pos += d[pos] + 1;
            pos++;
        } else if (d[pos] == 0x2C) break;
        else return 0;
    }
    if (pos + 10 > n) return 0;
    int ix = d[pos + 1] | (d[pos + 2] << 8), iy = d[pos + 3] | (d[pos + 4] << 8);
    int iw = d[pos + 5] | (d[pos + 6] << 8), ih = d[pos + 7] | (d[pos + 8] << 8), lf = d[pos + 9];
    pos += 10;
    if (lf & 0x80) { unsigned ls = 3u << ((lf & 7) + 1); if (pos + ls > n) return 0; memcpy(lpal, d + pos, ls); pos += ls; pal = lpal; }
    if (iw < 1 || ih < 1 || pos >= n) return 0;
    int mcs = d[pos++];
    if (mcs < 2 || mcs > 8) return 0;
    /* juntar sub-bloques */
    unsigned char *lz = malloc(n), *idx = malloc((size_t)iw * ih);
    unsigned short *prefix = malloc(4096 * 2); unsigned char *suffix = malloc(4096), *stack = malloc(4097);
    unsigned lzn = 0; int ok = 0;
    if (!lz || !idx || !prefix || !suffix || !stack) goto done;
    while (pos < n && d[pos]) { unsigned b = d[pos++]; if (pos + b > n) b = n - pos; memcpy(lz + lzn, d + pos, b); lzn += b; pos += b; }
    memset(idx, trans >= 0 ? trans : bg, (size_t)iw * ih);
    {
        int clear = 1 << mcs, eoi = clear + 1, avail = clear + 2, csize = mcs + 1, cmask = (1 << csize) - 1, old = -1, first = 0;
        unsigned bits = 0, acc = 0, bp = 0; int px = 0, total = iw * ih;
        for (i = 0; i < (unsigned)clear; i++) { prefix[i] = 0; suffix[i] = i; }
        while (px < total) {
            while (bits < (unsigned)csize && bp < lzn) { acc |= (unsigned)lz[bp++] << bits; bits += 8; }
            if (bits < (unsigned)csize) break;
            int code = acc & cmask; acc >>= csize; bits -= csize;
            if (code == clear) { csize = mcs + 1; cmask = (1 << csize) - 1; avail = clear + 2; old = -1; continue; }
            if (code == eoi) break;
            if (old < 0) { idx[px++] = suffix[code]; old = code; first = code; continue; }
            int in = code, sp = 0;
            if (code > avail) break;
            if (code == avail) { stack[sp++] = first; code = old; }
            while (code >= clear && sp < 4096) { stack[sp++] = suffix[code]; code = prefix[code]; }
            first = suffix[code]; stack[sp++] = first;
            if (avail < 4096) { prefix[avail] = old; suffix[avail] = first; avail++; if (!(avail & cmask) && avail < 4096) { csize++; cmask = (1 << csize) - 1; } }
            old = in;
            while (sp && px < total) idx[px++] = stack[--sp];
        }
    }
    {
        /* lienzo del tamano logico; lo transparente queda con el fondo del chat */
        unsigned char *rgb = malloc((size_t)sw * sh * 3);
        if (!rgb) goto done;
        int x, y;
        for (y = 0; y < sh; y++) for (x = 0; x < sw; x++) { unsigned char *q = rgb + (y * sw + x) * 3; q[0] = (C_BG2 >> 16) & 255; q[1] = (C_BG2 >> 8) & 255; q[2] = C_BG2 & 255; }
        int inter = lf & 0x40, row = 0, pass = 0;
        static const int st[4] = { 0, 4, 2, 1 }, inc[4] = { 8, 8, 4, 2 };
        for (y = 0; y < ih; y++) {
            int ry = y;
            if (inter) { ry = row; row += inc[pass]; while (row >= ih && pass < 3) { pass++; row = st[pass]; } }
            for (x = 0; x < iw; x++) {
                int c = idx[y * iw + x], dx = ix + x, dy = iy + ry;
                if (c == trans || dx >= sw || dy >= sh) continue;
                unsigned char *q = rgb + (dy * sw + dx) * 3;
                q[0] = pal[c * 3]; q[1] = pal[c * 3 + 1]; q[2] = pal[c * 3 + 2];
            }
        }
        ok = rgb_out(rgb, sw, sh, sw * 3, out);
        free(rgb);
    }
done:
    free(lz); free(idx); free(prefix); free(suffix); free(stack);
    return ok;
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
        if (cfg.input.has_animation) {   /* rc13: emotes animados -> primer cuadro */
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
