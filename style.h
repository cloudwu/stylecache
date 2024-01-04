#ifndef style_cache_h
#define style_cache_h

#include <stdint.h>
#include <stddef.h>

struct style_cache;
typedef struct { int idx; } style_handle_t;

struct style_attrib {
	void *data;	// input
	size_t sz;	// input
	uint8_t key;// input
	int change;	// output
};

typedef void * (*style_alloc) (void *ud, void *ptr, size_t osize, size_t nsize);

struct style_cache * style_newcache(const unsigned char inherit_mask[128], style_alloc alloc, void *ud);
void style_deletecache(struct style_cache *);
style_handle_t style_null(struct style_cache *);

style_handle_t style_create(struct style_cache *, int n, struct style_attrib a[]);
int style_modify(struct style_cache *, style_handle_t s, int n, struct style_attrib a[]);	// data == NULL means remove attrib, return 0 means not changed
int style_assign(struct style_cache *c, style_handle_t s, style_handle_t v);	// return 1 : dirty 0 : no change
void style_addref(struct style_cache *c, style_handle_t s);
void style_release(struct style_cache *c, style_handle_t s);

// Do not need to release handle from inherit
style_handle_t style_inherit(struct style_cache *, style_handle_t child, style_handle_t parent, int with_mask);

void style_flush(struct style_cache *);

void* style_find(struct style_cache *C, style_handle_t h, uint8_t key, size_t *sz);
void* style_index(struct style_cache *, style_handle_t h, int i, uint8_t *key, size_t *sz);

void style_dump_key(struct style_cache *C, style_handle_t h, uint8_t key, char fmt);

#endif
