// stb_image_impl.cpp - the single translation unit that compiles stb_image.
//
// It used to be src/rendering/loading_screen.cpp, which happened to be the
// first file that needed a PNG decoded. src/pipeline/asset_manager.cpp calls
// stbi_load too, so the decoder's only definition sat in a library above the
// one that uses it - measured on 2026-08-26 as the whole of the
// pipeline -> rendering back-edge, against fifty-nine symbols the other way.
//
// stb_image decodes an image format, which is what src/pipeline is for. No
// other file may define STB_IMAGE_IMPLEMENTATION.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
