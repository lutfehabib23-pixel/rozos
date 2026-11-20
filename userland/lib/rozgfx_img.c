/* rozOS — librozgfx: JPEG, PNG, GIF, TGA পড়া (ধাপ ১৪), stb_image দিয়ে
 * gfx_load_image() নিজের BMP/PPM পাঠক আগে চেষ্টা করে; না পারলে এখানে আসে। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "rozgfx.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_SIMD              /* SSE intrinsics লাগে না (TCC-বিল্ড প্রোগ্রামের সাথেও চলুক) */
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM               /* PPM নিজের পাঠক পড়ে */
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#include "stb_image.h"

gfx_surface *gfx_load_image_stb(const unsigned char *data, long size)
{
    int w, h, n, i;
    unsigned char *px;
    gfx_surface *s;
    if (size <= 0 || size > 0x7FFFFFFF) return NULL;
    px = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!px) return NULL;
    s = gfx_surface_new(w, h);
    if (s) {
        for (i = 0; i < w * h; i++) {
            const unsigned char *p = px + i * 4;
            s->px[i] = GFX_RGBA(p[0], p[1], p[2], p[3]);
        }
    }
    stbi_image_free(px);
    return s;
}

const char *gfx_image_error(void)
{
    return stbi_failure_reason();
}
