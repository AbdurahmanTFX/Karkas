/* stb_image's implementation for karkas-gui (docs/adr/0041): images of Markdown notes. Only the formats
   notes use are compiled in (less code reading untrusted bytes); files are read by the caller. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"
