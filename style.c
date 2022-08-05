#include "style.h"
#include "attrib.h"
#include "hash.h"
#include "combined_cache.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

#define ARENA_DEFAULT_BITS 10
#define HASH_STEP 5
#define HASH_MAXSTEP 5
#define MAX_KEY 128

#define DATA_NODE(handle) (((handle) & 1) == 0)

struct style_combine {
	uint64_t a;
	uint64_t b;
};

struct style_data {
	uint64_t id;
	attrib_t data;
	uint8_t removed;
	uint8_t dirty;
};

struct style_arena {
	struct style_data *h;
	int *dirty;
	int n;
	int shift;
	int dirty_n;
};

struct style_cache {
	struct style_arena arena;
	struct attrib_state *A;
	struct combined_cache cache;
	uint64_t lastid;
	unsigned char mask[MAX_KEY];
};

static inline uint64_t
new_id(struct style_cache *c) {
	c->lastid += 2;
	return c->lastid;
}

static void
style_arena_init(struct style_arena *arena, int bits) {
	int n = 1 << bits;
	arena->shift = 32 - bits;
	arena->n = n;
	arena->h = (struct style_data *)malloc(n * sizeof(struct style_data));
	arena->dirty = (int *)malloc(n * sizeof(int));
	arena->dirty_n = 0;
	int i;
	for (i=0;i<n;i++) {
		arena->h[i].id = 0;
	}
}

static void
style_arena_deinit(struct style_arena *arena) {
	free(arena->h);
}

static size_t
style_arena_memsize(struct style_arena *arena) {
	return (sizeof(struct style_data) + sizeof(int)) * arena->n;
}

struct style_cache *
style_newcache(const unsigned char inherit_mask[128]) {
	struct style_cache * c = (struct style_cache *)malloc(sizeof(*c));
	c->lastid = 0;
	style_arena_init(&c->arena, ARENA_DEFAULT_BITS);
	combined_cache_init(&c->cache);
	c->A = attrib_newstate(inherit_mask);
	return c;
}

void
style_deletecache(struct style_cache *c) {
	style_arena_deinit(&c->arena);
	attrib_close(c->A);
	free(c);
}

size_t
style_memsize(struct style_cache *C) {
	size_t sz = sizeof(*C);
	sz += style_arena_memsize(&C->arena);
	sz += attrib_memsize(C->A);

	return sz;
}

static int
insert_id(struct style_arena *arena, uint64_t id, attrib_t attr) {
	int slot = hash_mainslot(id64_hash(id), arena);
	int i;
	for (i=0;i<HASH_MAXSTEP;i++) {
		struct style_data *d = &arena->h[slot];
		if (d->id == 0 || d->removed) {
			d->id = id;
			d->removed = 0;
			d->dirty = 0;
			d->data = attr;
			return slot;
		}
		slot += HASH_STEP;
		if (slot >= arena->n)
			slot -= arena->n;
	}
	// rehash

	int n = arena->n;
	struct style_data *data = arena->h;
	int dirty_n = arena->dirty_n;
	int *dirty = arena->dirty;
	int bits = 32 - arena->shift;
	style_arena_init(arena, bits+1);

	for (i=0;i<n;i++) {
		if (data[i].id != 0 && !data[i].removed) {
			insert_id(arena, data[i].id, data[i].data);
		}
	}
	for (i=0;i<dirty_n;i++) {
		arena->dirty[i] = dirty[i];
	}
	arena->dirty_n = dirty_n;

	free(data);
	free(dirty);

	return insert_id(arena, id, attr);
}

static int
alloc_data(struct style_cache *c, attrib_t attr) {
	uint64_t id = new_id(c);
	return insert_id(&c->arena, id, attr);
}

static void
dealloc_data(struct style_cache *c, int index) {
	struct style_arena *arena = &c->arena;
	assert(index >= 0 && index < arena->n);
	struct style_data *d = &c->arena.h[index];
	assert(!d->removed);
	d->removed = 1;
	attrib_release(c->A, d->data);
}

static int
find_by_id(struct style_cache *c, uint64_t id) {
	struct style_arena *arena = &c->arena;
	int slot = hash_mainslot(id64_hash(id), arena);
	int i;
	for (i=0;i<HASH_MAXSTEP;i++) {
		struct style_data *d = &arena->h[slot];
		if (d->id == 0) {
			break;
		}
		if (d->id == id && !d->removed) {
			return slot;
		}
		slot += HASH_STEP;
		if (slot >= arena->n)
			slot -= arena->n;
	}
	return -1;
}

static inline style_handle_t
style_handle_from_attr(struct style_cache *C, attrib_t attr) {
	int index = alloc_data(C, attr);
	style_handle_t ret = { C->arena.h[index].id };
	return ret;
}

style_handle_t
style_create(struct style_cache *C, int n, struct style_attrib a[]) {
	struct attrib_state *A = C->A;
	assert(n <= MAX_KEY);
	int tmp[MAX_KEY];
	int i;
	for (i=0;i<n;i++) {
		tmp[i] = attrib_entryid(A, a[i].key, a[i].data, a[i].sz);
	}
	attrib_t attr = attrib_create(A, n, tmp);
	return style_handle_from_attr(C, attr);
}

static void
add_dirty(struct style_arena *arena, int index) {
	int n = arena->dirty_n ++;
	assert(n < arena->n);
	arena->dirty[n] = index;
}

int
style_modify(struct style_cache *C, style_handle_t s, int patch_n, struct style_attrib patch[]) {
	struct attrib_state *A = C->A;
	int index = find_by_id(C, s.idx);
	assert(index >= 0);
	struct style_data *d = &C->arena.h[index];
	int tmp[MAX_KEY];
	int n = attrib_get(A, d->data, tmp);
	int i;
	int removed = 0;
	int change = 0;
	for (i=0;i<patch_n;i++) {
		int index = attrib_find(A, d->data, patch[i].key);
		if (index < 0) {
			if (patch[i].data) {
				// new
				int kv = attrib_entryid(A, patch[i].key, patch[i].data, patch[i].sz);
				tmp[n++] = kv;
				assert(n <= MAX_KEY);
				patch[i].change = 1;
				change = 1;
			} else {
				patch[i].change = 0;
			}
		} else {
			if (patch[i].data) {
				// replace
				int kv = attrib_entryid(A, patch[i].key, patch[i].data, patch[i].sz);
				if (tmp[index] != kv) {
					change = 1;
					patch[i].change = 1;
					tmp[index] = kv;
				} else {
					patch[i].change = 0;
				}
			} else {
				// remove
				++removed;
				tmp[index] = -1;
				patch[i].change = 1;
				change = 1;
			}
		}
	}
	if (!change)
		return 0;

	int i2 = 0;
	int n2 = n;
	for (i=0;i<n && removed;i++) {
		if (tmp[i] >= 0) {
			tmp[i2] = tmp[i];
			++i2;
		} else {
			--n2;
		}
	}
	attrib_t new_attr = attrib_create(A, n2, tmp);
	if (!d->dirty) {
		d->dirty = 1;
		add_dirty(&C->arena, index);
	}
	attrib_release(A, d->data);
	d->data = new_attr;
	return 1;
}

void
style_release(struct style_cache *C, style_handle_t s) {
	int index = find_by_id(C, s.idx);
	assert(index >= 0);
	dealloc_data(C, index);
}

static int
eval_(struct style_cache *C, uint64_t handle) {
	if (DATA_NODE(handle)) {
		int index = find_by_id(C, handle);
		assert(index >= 0);
		struct style_data *d = &C->arena.h[index];
		return d->data.idx;
	}

	struct combined_node *node = combined_cache_find(&C->cache, handle);
	if (node == NULL) {
		return -1;
	}
	if (node->value) {
		return node->data.idx;
	}

	// transform node
	int child_id = eval_(C, node->a);
	if (child_id < 0)
		return -1;
	int parent_id = eval_(C, node->b);
	if (parent_id < 0)
		return -1;

	attrib_t child = { child_id };
	attrib_t parent = { parent_id };

	attrib_t result = attrib_inherit(C->A, child, parent, node->mask);
	node->value = 1;
	node->data = result;
	return result.idx;
}

style_handle_t
style_clone(struct style_cache *C, style_handle_t s) {
	if (s.idx == 0)
		return STYLE_NULL;
	int a = eval_(C, s.idx);
	if (a < 0)
		return STYLE_NULL;
	attrib_t attr = {a};
	return style_handle_from_attr(C, attrib_addref(C->A, attr));
}

static void
reset_dirty(struct style_arena *A) {
	int i;
	for (i=0;i<A->dirty_n;i++) {
		int index = A->dirty[i];
		A->h[index].dirty = 0;
	}
	A->dirty_n = 0;
}

static int check_node_dirty(struct style_cache *C, struct combined_node *node);

static int
check_handle_dirty(struct style_cache *C, uint64_t handle) {
	if (DATA_NODE(handle)) {
		int index = find_by_id(C, handle);
		if (index < 0)	// handle is not exist, so always dirty
			return 1;
		struct style_data *d = &C->arena.h[index];
		return d->dirty;
	} else {
		struct combined_node *node = combined_cache_find_notouch(&C->cache, handle);
		if (node == NULL) {
			return 1;
		}
		return check_node_dirty(C, node);
	}
}

static int
check_node_dirty(struct style_cache *C, struct combined_node *node) {
	if (node->value) {
		int dirty = check_handle_dirty(C, node->a);	
		dirty |= check_handle_dirty(C, node->b);
		if (dirty) {
			node->value = 0;
			attrib_release(C->A, node->data);
		}
		return dirty;
	} else {
		return 1;	// dirty
	}
}

void
style_flush(struct style_cache *C) {
	if (C->arena.dirty_n <= 0)
		return;
	int i;
	for (i=0;i<COMBINE_CACHE_SIZE;i++) {
		struct combined_node *node = &C->cache.queue[i];
		if (node->id != 0) {
			check_node_dirty(C, node);
		}
	}
	reset_dirty(&C->arena);
}

style_handle_t
style_inherit(struct style_cache *C, style_handle_t child, style_handle_t parent, int with_mask) {
	uint64_t id = combined_cache_new(&C->cache, child.idx, parent.idx, with_mask, &C->lastid, C->A);
	style_handle_t ret = { id };
	return ret;
}

int
style_eval(struct style_cache *C, style_handle_t handle) {
	return eval_(C, handle.idx);
}

void*
style_find(struct style_cache *C, int attrib_id, uint8_t key) {
	attrib_t a = { attrib_id };
	int index = attrib_find(C->A, a, key);
	if (index < 0)
		return NULL;
	return attrib_index(C->A, a, index, &key);
}

void*
style_index(struct style_cache *C, int attrib_id, int i, uint8_t *key) {
	attrib_t a = { attrib_id };
	return attrib_index(C->A, a, i, key);
}

void
style_dump(struct style_cache *C) {
	printf("attrib_state size = %zu\n", attrib_memsize(C->A));
	int n = C->arena.n;
	printf("arena n = %d\n", n);
	int i;
	for (i=0;i<n;i++) {
		struct style_data *d = &C->arena.h[i];
		if (d->id != 0 && !d->removed) {
			printf("\t[%d] id = %" PRIx64 "\tattrib = %x (%d)\n", i, d->id, d->data.idx, attrib_refcount(C->A, d->data));
		}
	}
	combined_cache_dump(&C->cache);
}

void
style_check(struct style_cache *C) {
	combined_cache_check(&C->cache);
}


#ifdef STYLE_TEST_MAIN

#define STR(s) s, sizeof(s"")

static void
print_handle(struct style_cache *C, style_handle_t handle) {
	int attrib = style_eval(C, handle);
	printf("HANDLE = %" PRIx64 ", attrib = %d\n", handle.idx, attrib);

	style_dump(C);

	int i;
	for (i=0;;i++) {
		uint8_t key;
		void* v = style_index(C, attrib, i, &key);
		if (v) {
			printf("\tKey = %d , Value = %s\n", key, (const char *)v);
		}
		else {
			break;
		}
	}

}

int
main() {
	unsigned char inherit_mask[MAX_KEY] = { 0 };
	struct style_cache * C = style_newcache(inherit_mask);

	struct style_attrib a[] = {
		{ STR("hello") ,1 },
		{ STR("world") ,2 },
	};

	style_handle_t h1 = style_create(C, sizeof(a)/sizeof(a[0]), a);

	struct style_attrib b[] = {
		{ STR("hello world"), 1 },
		{ STR("world"), 2 },
	};

	style_handle_t h2 = style_create(C, sizeof(b)/sizeof(b[0]), b);

	style_dump(C);

	style_check(C);


	printf("h1 = %" PRIx64 ", h2 = %" PRIx64 "\n", h1.idx, h2.idx);

	style_handle_t h3 = style_inherit(C, h1, h2, 0);

	style_dump(C);

	print_handle(C, h3);

	// Modify

	struct style_attrib patch[] = {
		{ NULL, 1 },	// remove 1
		{ STR("WORLD"), 2 },
	};

	style_modify(C, h1, sizeof(patch)/sizeof(patch[0]), patch);

	style_flush(C);

	h3 = style_clone(C, h3);

	print_handle(C, h3);

	style_dump(C);

	style_deletecache(C);

	return 0;
}
#endif
