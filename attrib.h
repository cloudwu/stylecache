#ifndef attrib_state_h
#define attrib_state_h

#include <stdint.h>

struct attrib_state;
typedef struct { int idx; } attrib_t;

struct attrib_state * attrib_newstate();
void attrib_close(struct attrib_state *);
size_t attrib_memsize(struct attrib_state *);

int attrib_entryid(struct attrib_state *, int key, void *ptr, size_t sz);
attrib_t attrib_create(struct attrib_state *, int n, const int e[]);	// Notice: entryid can be invalid after create
void attrib_release(struct attrib_state *, attrib_t);
int attrib_get(struct attrib_state *, attrib_t, int output[128]);	// key is [0,127]
int attrib_find(struct attrib_state *, attrib_t, uint8_t key);	// -1 == not found
void* attrib_index(struct attrib_state *, attrib_t, int i, uint8_t *key);
attrib_t attrib_inherit(struct attrib_state *, attrib_t child, attrib_t parent, const unsigned char * inherit_mask);	// inherit_mask can be NULL
attrib_t attrib_addref(struct attrib_state *, attrib_t a);
int attrib_refcount(struct attrib_state *, attrib_t a);

#endif
