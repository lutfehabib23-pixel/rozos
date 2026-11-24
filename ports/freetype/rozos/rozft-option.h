/* rozOS — FreeType-এর বিকল্প (ধাপ ২৩ক): আসল ftoption.h, তারপর বাইরের লাইব্রেরি বন্ধ */
#ifndef ROZFT_OPTION_H
#define ROZFT_OPTION_H
#include <freetype/config/ftoption.h>
#undef FT_CONFIG_OPTION_USE_ZLIB              /* zlib/bzip2/png/brotli/harfbuzz লাগে না */
#undef FT_CONFIG_OPTION_USE_BZIP2
#undef FT_CONFIG_OPTION_USE_PNG
#undef FT_CONFIG_OPTION_USE_HARFBUZZ
#undef FT_CONFIG_OPTION_USE_BROTLI
#undef FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES  /* getenv("FREETYPE_PROPERTIES") নয় */
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_DEBUG_LEVEL_ERROR
#undef FT_DEBUG_LEVEL_TRACE
#endif
