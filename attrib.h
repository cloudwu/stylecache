#ifndef attrib_state_h
#define attrib_state_h

#include <stdint.h>
#include <stddef.h>
#include "style_alloc.h"

struct attrib_state;
typedef struct { int idx; } attrib_t;

struct attrib_state * attrib_newstate(const unsigned char inherit_mask[128], struct style_cache *C);
void attrib_close(struct attrib_state *, struct style_cache *C);

int attrib_entryid(struct attrib_state *, int key, void *ptr, size_t sz, struct style_cache *C);
attrib_t attrib_create(struct attrib_state *, int n, const int e[], struct style_cache *C);	// Notice: entryid can be invalid after create
int attrib_release(struct attrib_state *, attrib_t, struct style_cache *C);
int attrib_get(struct attrib_state *, attrib_t, int output[128]);	// key is [0,127]
int attrib_find(struct attrib_state *, attrib_t, uint8_t key);	// -1 == not found
int attrib_index(struct attrib_state *A, attrib_t handle, int i);
attrib_t attrib_inherit(struct attrib_state *, attrib_t child, attrib_t parent, int with_mask, struct style_cache *C);
attrib_t attrib_addref(struct attrib_state *, attrib_t a);
int attrib_refcount(struct attrib_state *, attrib_t a);

void* attrib_entry_get(struct attrib_state *A, int id, uint8_t *key, size_t *sz);
void attrib_entry_addref(struct attrib_state *A, int id);
void attrib_entry_release(struct attrib_state *A, int id, struct style_cache *C);

#endif
