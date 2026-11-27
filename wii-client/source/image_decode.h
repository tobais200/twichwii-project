#ifndef IMAGE_DECODE_H
#define IMAGE_DECODE_H
/* Bounded network image decoder. Output is software-canvas 0x00RRGGBB. */
int image_decode(const unsigned char *data, unsigned size, kimg_t *out);
/* cuadros reducidos (alto th) de un GIF/WebP animado; NULL si no es animado o no entra
 * en maxbytes (se bajan cuadros si hay muchos). */
kanim_t *image_anim(const unsigned char *d, unsigned n, int th, int maxw, int maxbytes, int maxf);
void anim_free(kanim_t *a);
#endif
