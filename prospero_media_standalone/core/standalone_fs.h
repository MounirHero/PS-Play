#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal file reader required by the embedded BigApp loader.
 * This intentionally replaces websrv's libmicrohttpd-aware fs.h.
 */
uint8_t* fs_readfile(const char* path, size_t* size);
