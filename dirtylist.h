#ifndef dirty_list_h
#define dirty_list_h

#include "style_alloc.h"

struct dirtylist;

struct dirtylist * dirtylist_expand(struct dirtylist *, struct style_cache *C);
void dirtylist_release(struct dirtylist *, struct style_cache *C);
int dirtylist_add(struct dirtylist *, int a, int b, int next);
void dirtylist_clear(struct dirtylist *, int a);
int* dirtylist_next(struct dirtylist *, int *index, int *value);
void dirtylist_dump(struct dirtylist *);
void dirtylist_check(struct dirtylist *, int index, int v);

#endif
