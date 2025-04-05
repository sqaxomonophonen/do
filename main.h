#ifndef MAIN_H

// host/main interface

#include <stdint.h>

// returns number of nanoseconds since program started
int64_t get_nanoseconds(void);

#define TT(s)       ((s)<<2)
#define TTMASK(s)   (TT((s)+1)-1)
#define TTGET(v,s)  (((v)>>TT(s))&TTMASK(s))

#define TT_LUMEN8    (0<<TT(0))
#define TT_RGBA8888  (1<<TT(0))

#define TT_SMOOTH    (0<<TT(1))
#define TT_PIXELATED (1<<TT(1))

#define TT_STATIC    (0<<TT(2))
#define TT_STREAM    (1<<TT(2))

int create_texture(int type, int width, int height);
void get_texture_dim(int texture, int* out_width, int* out_height);
void destroy_texture(int texture);
void update_texture(int texture, int y0, int width, int height, void* data);


#define MAIN_H
#endif
