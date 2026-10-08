// Host stand-in for esp_rom/include/miniz.h: the miniz release the ESP32-S3 ROM carries (MZ_VERSION
// "9.1.15"), from source (vendor/sources.yml), with the feature switches the ROM header sets.
//
// One difference stays: on a 64-bit host miniz picks a 64-bit bit buffer inside tinfl, the ROM a
// 32-bit one. The inflate is the same; the board run is what proves the ROM itself.
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_MALLOC
#define MINIZ_HEADER_FILE_ONLY
#include "vendor/miniz/miniz.c"
#undef MINIZ_HEADER_FILE_ONLY
