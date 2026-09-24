/* stb_image implementation for the music player: JPEG + PNG from memory
 * only, no stdio, no HDR / linear float paths, no SIMD. Built quietly
 * (third-party code) through QUIET_SRC. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void) 0)
#include "third_party/stb_image.h"
