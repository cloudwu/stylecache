#ifndef style_cache_h
#define style_cache_h

#include <stdint.h>
#include <stddef.h>

struct style_cache;
typedef struct { uint64_t idx; } style_handle_t;
static const style_handle_t STYLE_NULL = { 0 };

struct style_attrib {
	void *data;	// input
	size_t sz;	// input
	uint8_t key;// input
	int change;	// output
};

struct style_cache * style_newcache(const unsigned char inherit_mask[128]);
void style_deletecache(struct style_cache *);
size_t style_memsize(struct style_cache *);
void style_dump(struct style_cache *);	// for debug

style_handle_t style_create(struct style_cache *, int n, struct style_attrib a[]);
int style_modify(struct style_cache *, style_handle_t s, int n, struct style_attrib a[]);	// data == NULL means remove attrib, return 0 means not changed
style_handle_t style_clone(struct style_cache *, style_handle_t s);
void style_release(struct style_cache *c, style_handle_t s);

// Do not need to release handle from inherit
style_handle_t style_inherit(struct style_cache *, style_handle_t child, style_handle_t parent, int with_mask);

void style_flush(struct style_cache *);	// must call flush before style_eval and after style_modify

int style_eval(struct style_cache *, style_handle_t);
void* style_find(struct style_cache *, int attrib, uint8_t key);
void* style_index(struct style_cache *, int attrib, int i, uint8_t *key);
void style_check(struct style_cache *);

#endif
