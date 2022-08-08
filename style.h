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

struct style_cache * style_newcache(const unsigned char inherit_mask[128]);
void style_deletecache(struct style_cache *);
size_t style_memsize(struct style_cache *);

style_handle_t style_create(struct style_cache *, int n, struct style_attrib a[]);
int style_modify(struct style_cache *, style_handle_t s, int n, struct style_attrib a[]);	// data == NULL means remove attrib, return 0 means not changed
void style_assign(struct style_cache *c, style_handle_t s, style_handle_t v);
void style_addref(struct style_cache *c, style_handle_t s);
void style_release(struct style_cache *c, style_handle_t s);

// Do not need to release handle from inherit
style_handle_t style_ref(struct style_cache *);
style_handle_t style_inherit(struct style_cache *, style_handle_t child, style_handle_t parent, int with_mask);

void style_flush(struct style_cache *);

void* style_find(struct style_cache *C, style_handle_t h, uint8_t key);
void* style_index(struct style_cache *, style_handle_t h, int i, uint8_t *key);

#endif
