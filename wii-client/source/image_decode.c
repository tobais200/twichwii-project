#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
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
typedef struct { struct jpeg_error_mgr pub; jmp_buf jump; unsigned char *rgb; } jpeg_err;
static void jpeg_fail(j_common_ptr c) { longjmp(((jpeg_err *)c->err)->jump, 1); }
static int from_jpeg(const unsigned char *data, unsigned size, kimg_t *out) {
    struct jpeg_decompress_struct c; jpeg_err e;
    memset(&c, 0, sizeof(c)); memset(&e, 0, sizeof(e));
    c.err = jpeg_std_error(&e.pub); e.pub.error_exit = jpeg_fail;
    if (setjmp(e.jump)) { free(e.rgb); jpeg_destroy_decompress(&c); return 0; }
    jpeg_create_decompress(&c); jpeg_mem_src(&c, data, size);
    jpeg_read_header(&c, TRUE);
    if (!c.image_width || !c.image_height || c.image_width > 2048 || c.image_height > 2048) { jpeg_destroy_decompress(&c); return 0; }
    c.out_color_space = JCS_RGB;
    c.scale_num = 1; c.scale_denom = 1;
    while (c.scale_denom < 8 && c.image_width / c.scale_denom > 640) c.scale_denom *= 2;
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
    png_set_read_fn(p,st,png_memread);png_read_info(p,info);
    int w=png_get_image_width(p,info),h=png_get_image_height(p,info);
    if(w<1||h<1||w>4096||h>4096||(unsigned)w*h>8*1024*1024) png_error(p,"image too large");
    int type=png_get_color_type(p,info),depth=png_get_bit_depth(p,info);
    if(depth==16)png_set_strip_16(p);
    if(type==PNG_COLOR_TYPE_PALETTE)png_set_palette_to_rgb(p);
    if(type==PNG_COLOR_TYPE_GRAY&&depth<8)png_set_expand_gray_1_2_4_to_8(p);
    if(png_get_valid(p,info,PNG_INFO_tRNS))png_set_tRNS_to_alpha(p);
    if(type==PNG_COLOR_TYPE_GRAY||type==PNG_COLOR_TYPE_GRAY_ALPHA)png_set_gray_to_rgb(p);
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
int image_decode(const unsigned char *data, unsigned size, kimg_t *out) {
    if (size < 12) return 0;
    if (data[0] == 0xff && data[1] == 0xd8) return from_jpeg(data, size, out);
    if (!png_sig_cmp(data, 0, 8)) return from_png(data,size,out);
    if (!memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) {
        WebPDecoderConfig cfg;
        if (!WebPInitDecoderConfig(&cfg) || WebPGetFeatures(data, size, &cfg.input) != VP8_STATUS_OK) return 0;
        if (cfg.input.width > 2048 || cfg.input.height > 2048 || cfg.input.has_animation) return 0;
        int w, h; dims(cfg.input.width, cfg.input.height, &w, &h);
        cfg.options.use_scaling = 1; cfg.options.scaled_width = w; cfg.options.scaled_height = h;
        cfg.output.colorspace = MODE_RGB;
        int ok = WebPDecode(data, size, &cfg) == VP8_STATUS_OK;
        if (ok) ok = rgb_out(cfg.output.u.RGBA.rgba, w, h, cfg.output.u.RGBA.stride, out);
        WebPFreeDecBuffer(&cfg.output); return ok;
    }
    return 0;
}
