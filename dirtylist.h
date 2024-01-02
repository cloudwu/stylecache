#ifndef dirty_list_h
#define dirty_list_h

#include "style_alloc.h"

struct dirtylist;

struct dirtylist * dirtylist_create(struct style_cache *C);
void dirtylist_release(struct dirtylist *);
void dirtylist_add(struct dirtylist *, int a, int b);
void dirtylist_clear(struct dirtylist *, int a);
int dirtylist_get(struct dirtylist *, int id, int n, int *output);
void dirtylist_dump(struct dirtylist *);

#endif
