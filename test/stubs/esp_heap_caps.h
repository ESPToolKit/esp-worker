#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_8BIT 0x1
#define MALLOC_CAP_INTERNAL 0x2
#define MALLOC_CAP_SPIRAM 0x4

void *heap_caps_malloc(size_t size, unsigned int caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_total_size(unsigned int caps);

#ifdef __cplusplus
}
#endif
