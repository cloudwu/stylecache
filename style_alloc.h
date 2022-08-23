#ifndef style_allocator_h
#define style_allocator_h

#include <stddef.h>

struct style_cache;

void * style_malloc(struct style_cache *c, size_t size);
void style_free(struct style_cache *c, void *ptr, size_t osize);
void * style_realloc(struct style_cache *c, void *ptr, size_t osize, size_t nsize);

#endif
