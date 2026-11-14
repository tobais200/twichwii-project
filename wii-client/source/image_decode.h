#ifndef IMAGE_DECODE_H
#define IMAGE_DECODE_H
/* Bounded network image decoder. Output is software-canvas 0x00RRGGBB. */
int image_decode(const unsigned char *data, unsigned size, kimg_t *out);
#endif
