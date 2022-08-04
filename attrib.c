#include "attrib.h"
#include "hash.h"
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define DEFAULT_ATTRIB_ARENA_SIZE 128
#define DEFAULT_TUPLE_SIZE 128
#define ATTRIB_ARRAY_STACK_SIZE 64
#define EMBED_VALUE_SIZE 8
#define EMBED_HASH_SLOT 7
#define HASH_STEP 5
#define HASH_INIT_BITS 7
#define HASH_MAXSTEP 5
#define MAX_KEY 128
#define DELAY_REMOVE 4096

struct attrib_blob {
	size_t sz;
	uint8_t data[1];
};

struct attrib_kv {
	uint32_t blob:1;
	uint32_t k:7;
	uint32_t refcount:24;
	uint32_t hash;
	union {
		uint8_t buffer[EMBED_VALUE_SIZE];
		struct attrib_blob *ptr;
		int next;
	} v;
};

struct attrib_arena {
	int n;
	int cap;
	int freelist;
	struct attrib_kv *e;
};

// { -1, 0 } : empty
// { -1, n } : extra array of n
// { index, ... , -1 }
struct attrib_hash_entry {
	uint32_t hash;
	int index[EMBED_HASH_SLOT];
};

struct attrib_lookup {
	int n;
	int shift;
	struct attrib_hash_entry *e;
};

struct attrib_array {
	int refcount;
	int n;
	uint32_t hash;
	int data[1];
};

union attrib_tuple_entry {
	struct attrib_array *a;
	int next;
};

struct attrib_tuple {
	int n;
	int cap;
	int freelist;
	union attrib_tuple_entry *s;
};

// hash 0 is invalid slot
// index -1 is removed slot
struct attrib_tuple_hash_entry {
	uint32_t hash;
	int index;
};

struct attrib_tuple_lookup {
	int n;
	int shift;
	struct attrib_tuple_hash_entry *e;
};

#define INHERIT_ID_MAX (1<<20)
#define INHERIT_CACHE_SIZE (1<<13)// 8192, 13bits
#define INHERIT_CACHE_SHIFT (32-13)
#define INHERIT_MAX_COUNT ((1<<11)-1)

// 64bits
struct inherit_entry {
	uint64_t a : 20;
	uint64_t b : 20;
	uint64_t result : 20;
	uint64_t withmask : 1;
	uint64_t valid : 1;
};

struct inherit_key {
	uint32_t key : 20;
	uint32_t count : 11;
	uint32_t valid : 1;
};

struct inherit_cache {
	struct inherit_entry s[INHERIT_CACHE_SIZE];
	struct inherit_key key[INHERIT_CACHE_SIZE];
};

struct attrib_state {
	struct attrib_arena arena;
	struct attrib_lookup arena_l;
	struct attrib_tuple tuple;
	struct attrib_tuple_lookup tuple_l;
	struct inherit_cache icache;
	int remove_head;
	int remove_tail;
	int removed[DELAY_REMOVE];
	unsigned char inherit_mask[128];
	int kv_used;
	int kv_n;
};

static void
inherit_cache_init(struct inherit_cache *c) {
	memset(c, 0, sizeof(*c));
}

static inline int
hash_inherit_key(int a) {
	uint32_t v = (uint32_t)a;
	v = int32_hash(v) >> INHERIT_CACHE_SHIFT;
	return v;
}

static inline int
inherit_cache_key_valid(struct inherit_cache *c, int a) {
	if (a >= INHERIT_ID_MAX)
		return 0;
	struct inherit_key *k = &c->key[hash_inherit_key(a)];
	if (k->key != a)
		return 0;
	if (k->count == 0 || !k->valid)
		return 0;

	return 1;
}

static int
inherit_cache_fetch(struct inherit_cache *c, int a, int b, int withmask) {
	if (!inherit_cache_key_valid(c, a) || !inherit_cache_key_valid(c, a))
		return -1;
	uint32_t v = (a & 0xffff) | ((b & 0xffff) << 16);
	v = v * 0xdeece66d + 0xb;
	v %= INHERIT_CACHE_SIZE;
	struct inherit_entry *e = &c->s[v];
	if (e->valid && e->a == a && e->b == b && e->withmask == withmask)
		return e->result;
	return -1;
}

static inline void
unset_key(struct inherit_cache *c, int a) {
	struct inherit_key *k = &c->key[hash_inherit_key(a)];
	assert(k->key == a && k->count >0);
	--k->count;
}

static inline int
set_key(struct inherit_cache *c, int key) {
	struct inherit_key *k = &c->key[hash_inherit_key(key)];
	if (!k->valid) {
		if (k->count > 0) {
			// clear entries with key
			int i;
			for (i=0;i<INHERIT_CACHE_SIZE;i++) {
				struct inherit_entry *e = &c->s[i];
				if (e->valid && (e->a == key || e->b == key || e->result == key)) {
					e->valid = 0;
					unset_key(c, e->a);
					unset_key(c, e->b);
					unset_key(c, e->result);
				}
			}
		}
		k->key = key;
		k->count = 1;
		k->valid = 1;
		return 1;	// succ
	} else {
		if (k->key != key)
			return 0;	// fail
		if (k->count >= INHERIT_MAX_COUNT)
			return 0;	// too many, fail
		++k->count;
		return 1;
	}
}

static inline void
inherit_cache_retirekey(struct inherit_cache *c, int key) {
	struct inherit_key *k = &c->key[hash_inherit_key(key)];
	if (k->key == key) {
		k->valid = 0;
	}
}

// Cache a,b and result in inherit_cache
// If cache failed, don't set keys in inherit_cache, otherwise, addref in it.
static void
inherit_cache_set(struct inherit_cache *c, int a, int b, int withmask, int result) {
	uint32_t v = (a & 0xffff) | ((b & 0xffff) << 16);
	v = v * 0xdeece66d + 0xb;
	v %= INHERIT_CACHE_SIZE;
	struct inherit_entry *e = &c->s[v];
	if (e->valid) {
		unset_key(c, e->a);
		unset_key(c, e->b);
		unset_key(c, e->result);
		e->valid = 0;
	}
	e->a = a;
	e->b = b;
	e->result = result;
	e->withmask = withmask;

	if (!set_key(c, a))
		return;

	if (!set_key(c, b)) {
		unset_key(c, a);
		return;
	}

	if (!set_key(c, result)) {
		unset_key(c, a);
		unset_key(c, b);
		return;
	}

	e->valid = 1;
}

static size_t
arena_size(struct attrib_arena *arena) {
	size_t sz = arena->cap * sizeof(struct attrib_kv);
	int i;
	for (i=0;i<arena->n;i++) {
		if (arena->e[i].blob) {
			sz += arena->e[i].v.ptr->sz + sizeof(struct attrib_blob) - 1;
		}
	}
	return sz;
}

static size_t
arena_l_size(struct attrib_lookup *h) {
	size_t sz = h->n * sizeof(struct attrib_hash_entry);
	int i;
	for (i=0;i<h->n;i++) {
		struct attrib_hash_entry * e = &h->e[i];
		if (e->index[0] == -1) {
			int n = e->index[1];
			if (n > 0) {
				sz += sizeof(int) * n;
			}
		}
	}
	return sz;
}

static size_t
tuple_size(struct attrib_state *A) {
	struct attrib_tuple *tuple = &A->tuple;
	size_t sz = tuple->cap * sizeof(union attrib_tuple_entry);
	struct attrib_tuple_lookup *h = &A->tuple_l;
	sz += h->n * sizeof(struct attrib_tuple_hash_entry);
	int i;
	for (i=0;i<h->n;i++) {
		struct attrib_tuple_hash_entry *e = &h->e[i];
		if (e->hash != 0 && e->index >= 0) {
			struct attrib_array *a = tuple->s[e->index].a;
			sz += sizeof(*a) + a->n * sizeof(int) - sizeof(int);
		}
	}
	return sz;
}

size_t
attrib_memsize(struct attrib_state *A) {
	size_t sz = sizeof(*A);
	sz += arena_size(&A->arena);
	sz += arena_l_size(&A->arena_l);
	sz += tuple_size(A);
	return sz;
}

static inline int *
hashvalue_ptr(struct attrib_hash_entry *e) {
	if (e->index[0] == -1) {
		if (e->index[1] == -1)
			return NULL;	// invalid entry
		else {
			int *ret;
			memcpy(&ret, &e->index[2], sizeof(int *));
			return ret;
		}
	} else {
		return e->index;
	}
}

static void
tuple_init(struct attrib_tuple *tuple) {
	tuple->n = 0;
	tuple->cap = DEFAULT_TUPLE_SIZE;
	tuple->s = (union attrib_tuple_entry *)malloc(DEFAULT_TUPLE_SIZE * sizeof(union attrib_tuple_entry));
	tuple->freelist = -1;
}

static void
tuple_deinit(struct attrib_tuple *tuple) {
	int node = tuple->freelist;
	while (node >= 0) {
		int current = node;
		node = tuple->s[current].next;
		tuple->s[current].a = NULL;
	}
	int i;
	for (i=0;i<tuple->n;i++) {
		free(tuple->s[i].a);
	}
	free(tuple->s);
}

static int
tuple_new(struct attrib_tuple *tuple, struct attrib_array *data) {
	int index = tuple->freelist;
	if (index >= 0) {
		tuple->freelist = tuple->s[index].next;
	} else {
		index = tuple->n++;
		if (index >= tuple->cap) {
			int newcap = tuple->cap * 3 / 2;
			tuple->s = (union attrib_tuple_entry *)realloc(tuple->s, newcap * sizeof(union attrib_tuple_entry));
			assert(tuple->s != NULL);
			tuple->cap = newcap;
		}
	}
	tuple->s[index].a = data;
	return index;
}

static void
tuple_delete(struct attrib_tuple *tuple, int index) {
	assert(index >=0 && index < tuple->n);
	free(tuple->s[index].a);
	tuple->s[index].a = NULL;
	tuple->s[index].next = tuple->freelist;
	tuple->freelist = index;
}

static void
arena_init(struct attrib_arena *arena) {
	arena->e = (struct attrib_kv *)malloc(DEFAULT_ATTRIB_ARENA_SIZE * sizeof(struct attrib_kv));
	arena->n = 0;
	arena->cap = DEFAULT_ATTRIB_ARENA_SIZE;
	arena->freelist = -1;
}

static void
arena_deinit(struct attrib_arena *arena) {
	int i;
	for (i=0;i<arena->n;i++) {
		if (arena->e[i].blob) {
			free(arena->e[i].v.ptr);
		}
	}
	free(arena->e);
}

static struct attrib_blob *
blob_new(void *ptr, size_t sz) {
	struct attrib_blob * b = (struct attrib_blob *)malloc(sizeof(*b) - 1 + sz);
	b->sz = sz;
	memcpy(b->data, ptr, sz);
	return b;
}

static int
arena_create(struct attrib_arena *arena, int key, void *value, size_t sz, uint32_t hash) {
	struct attrib_kv *kv;
	int index;
	if (arena->freelist >= 0) {
		index = arena->freelist;
		kv = &arena->e[index];
		arena->freelist = kv->v.next;
	} else {
		if (arena->n >= arena->cap) {
			int newcap = arena->cap * 3 / 2;
			arena->e = (struct attrib_kv *)realloc(arena->e, newcap * sizeof(struct attrib_kv));
			assert(arena->e != NULL);
			arena->cap = newcap;
		}
		index = arena->n++;
		kv = &arena->e[index];
	}
	assert(key >= 0 && key <=127);
	kv->k = key;
	kv->refcount = 0;
	kv->hash = hash;
	if (sz > EMBED_VALUE_SIZE) {
		kv->blob = 1;
		kv->v.ptr = blob_new(value, sz);
	} else {
		kv->blob = 0;
		kv->v.ptr = NULL;
		memcpy(kv->v.buffer, value, sz);
	}
	return index;
}

static int
arena_release(struct attrib_arena *arena, int id) {
	assert(id >= 0 && id < arena->n);
	struct attrib_kv * kv = &arena->e[id];
	assert(kv->refcount > 0);
	int c = --kv->refcount;
	if (c == 0) {
		if (kv->blob) {
			free(kv->v.ptr);
			kv->blob = 0;
			kv->refcount = 1;	// avoid collect
		}
		kv->v.next = arena->freelist;
		arena->freelist = id;
	}
	return c;
}

static int
arena_addref(struct attrib_arena *arena, int id) {
	assert(id >= 0 && id < arena->n);
	struct attrib_kv * kv = &arena->e[id];
	++kv->refcount;
	assert(kv->refcount != 0);
	return kv->refcount;
}

static void
init_slots(struct attrib_lookup *h, int bits) {
	int n = 1 << bits;
	h->e = (struct attrib_hash_entry *)malloc(n * sizeof(struct attrib_hash_entry));
	h->n = n;
	h->shift = 32 - bits;
	int i;
	for (i=0;i<n;i++) {
		// set invalid
		h->e[i].index[0] = -1;
		h->e[i].index[1] = -1;
	}
}

static void
hash_init(struct attrib_lookup *h) {
	init_slots(h, HASH_INIT_BITS);
}

static void
free_entry(struct attrib_hash_entry *e, int n) {
	int i;
	for (i=0;i<n;i++) {
		int *p = hashvalue_ptr(&e[i]);
		if (p && p != e[i].index) {
			free(p);
		}
	}
	free(e);
}

static void
hash_deinit(struct attrib_lookup *h) {
	free_entry(h->e, h->n);
}

static void
init_tuple_lut_slots(struct attrib_tuple_lookup *lut, int bits) {
	int n = 1 << bits;
	lut->e = (struct attrib_tuple_hash_entry *)malloc(n * sizeof(struct attrib_tuple_hash_entry));
	lut->n = n;
	lut->shift = 32 - bits;
	int i;
	for (i=0;i<n;i++) {
		// set invalid
		lut->e[i].hash = 0;
	}
}

static int
check_tuple(struct attrib_state *A, int index, int n, const int a[]) {
	struct attrib_array *array = A->tuple.s[index].a;
	if (n != array->n)
		return 0;
	return memcmp(a, array->data, n * sizeof(int)) == 0;
}

static int
tuple_hash_find(struct attrib_state *A, uint32_t hash, int n, const int a[]) {
	struct attrib_tuple_lookup *h = &A->tuple_l;
	int slot = hash_mainslot(hash, h);
	struct attrib_tuple_hash_entry *e = &h->e[slot];
	if (e->hash == 0)
		return -1;
	if (e->hash == hash && e->index != -1) {
		if (check_tuple(A, e->index, n, a))
			return e->index;
	}
	int i;
	for (i=0;i<HASH_MAXSTEP;i++) {
		slot += HASH_STEP;
		if (slot >= h->n)
			slot -= h->n;
		e = &h->e[slot];
		if (e->hash == 0)
			return -1;
		if (e->hash == hash && e->index != -1) {
			if (check_tuple(A, e->index, n, a))
				return e->index;
		}
	}
	return -1;
}

static void
tuple_hash_insert(struct attrib_tuple_lookup *h, uint32_t hash, int index) {
	int slot = hash_mainslot(hash, h);
	int i;
	for (i=0;i<HASH_MAXSTEP;i++) {
		struct attrib_tuple_hash_entry *e = &h->e[slot];
		if (e->hash == 0 || e->index == -1) {
			e->hash = hash;
			e->index = index;
			return;
		}
		slot += HASH_STEP;
		if (slot >= h->n)
			slot -= h->n;
	}
	// rehash
	struct attrib_tuple_hash_entry *e = h->e;
	int n = h->n;
	int bits = 32 - h->shift;

	init_tuple_lut_slots(h, bits+1);
	for (i=0;i<n;i++) {
		if (e[i].hash != 0 && e[i].index >= 0) {
			tuple_hash_insert(h, e[i].hash, e[i].index);
		}
	}
	free(e);
	tuple_hash_insert(h, hash, index);
}

static void
tuple_hash_remove(struct attrib_tuple_lookup *h, uint32_t hash, int index) {
	int slot = hash_mainslot(hash, h);
	for (;;) {
		struct attrib_tuple_hash_entry *e = &h->e[slot];
		if (e->hash == hash && e->index == index) {
			e->index = -1;
			return;
		}
		assert(e->hash != 0);
		slot += HASH_STEP;
		if (slot >= h->n)
			slot -= h->n;
	}
}

static void
tuple_hash_init(struct attrib_tuple_lookup *lut) {
	init_tuple_lut_slots(lut, HASH_INIT_BITS);
}

static void
tuple_hash_deinit(struct attrib_tuple_lookup *lut) {
	free(lut->e);
}

// extra space 
// [0] : -1
// [1] : n
// [2/3] : pointer (64bits)
static int *
expand_extraspace(struct attrib_hash_entry * e) {
	int *extra;
	if (e->index[0] == -1) {
		int n = e->index[1];
		extra = hashvalue_ptr(e);
		int newcap = n * 3 / 2;
		extra = (int *)realloc(extra, newcap * sizeof(int));
		e->index[1] = newcap;
	} else {
		extra = (int *)malloc(sizeof(int) * (EMBED_HASH_SLOT + 1));
		memcpy(extra, e->index, EMBED_HASH_SLOT * sizeof(int));
		e->index[0] = -1;
		e->index[1] = EMBED_HASH_SLOT + 1;
	}
	memcpy(&e->index[2], &extra, sizeof(extra));
	return extra;
}

static struct attrib_hash_entry * find_slot(struct attrib_lookup *h, uint32_t hash);
static void attrib_hash_insert(struct attrib_lookup *h, uint32_t hash, int index);

static void
rehash(struct attrib_lookup *h) {
	struct attrib_hash_entry *e = h->e;
	int n = h->n;
	int bits = 32 - h->shift;
	init_slots(h, bits + 1);
	int i,j;
	for (i=0;i<n;i++) {
		int * p = hashvalue_ptr(&e[i]);
		if (p) {
			int size = (p == e[i].index) ? EMBED_HASH_SLOT : e[i].index[1];
			for (j=0;j<size && p[j] >= 0;j++) {
				attrib_hash_insert(h, e[i].hash, p[j]);
			}
		}
	}
	free_entry(e, n);
}

static struct attrib_hash_entry *
find_nextslot(struct attrib_lookup *h, uint32_t hash) {
	int i;
	int index = hash_mainslot(hash, h);
	for (i=0;i<HASH_MAXSTEP;i++) {
		index += HASH_STEP;
		if (index >= h->n)
			index -= h->n;
		struct attrib_hash_entry * e = &h->e[index];
		if (e->hash == hash || hashvalue_ptr(e) == NULL)
			return e;
	}
	rehash(h);

	return find_slot(h, hash);
}

static struct attrib_hash_entry *
find_slot(struct attrib_lookup *h, uint32_t hash) {
	int mainslot = hash_mainslot(hash, h);
	struct attrib_hash_entry * e = &h->e[mainslot];
	if (e->hash == hash || hashvalue_ptr(e) == NULL)
		return e;
	return find_nextslot(h, hash);
}

static void
attrib_hash_insert(struct attrib_lookup *h, uint32_t hash, int index) {
	assert(index >= 0);
	struct attrib_hash_entry * e = find_slot(h, hash);
	int * v = hashvalue_ptr(e);
	if (v == NULL) {
		e->hash = hash;
		e->index[0] = index;
		e->index[1] = -1;
		return;
	}
	if (e->hash != hash) {
		e = find_nextslot(h, hash);	// may rehash
	}
	int n = EMBED_HASH_SLOT;
	if (v != e->index) {
		// extra space
		n = e->index[1];
	}
	int i;
	for (i=0;i < n && v[i] != -1;i++) {
		if (v[i] == index)
			return;
	}
	if (i < n) {
		v[i] = index;
		if (i < n - 1) {
			v[i+1] = -1;
		}
	} else {
		v = expand_extraspace(e);
		v[n] = index;
		v[n+1] = -1;
	}
}

static void
attrib_hash_remove(struct attrib_lookup *h, uint32_t hash, int index) {
	assert(index >= 0);
	struct attrib_hash_entry * e = find_slot(h, hash);
	int * v = hashvalue_ptr(e);
	assert(v != NULL);
	int n = (v == e->index) ? EMBED_HASH_SLOT : e->index[1];
	int i;
	int removed = -1;
	for (i=0;i<n;i++) {
		if (v[i] == -1) {
			break;
		}
		if (v[i] == index)
			removed = i;
	}
	assert(removed >= 0);
	int last = i - 1;
	v[removed] = v[last];
	v[last] = -1;
}

static int *
attrib_hash_lookup(struct attrib_lookup *h, uint32_t hash, int *n) {
	struct attrib_hash_entry * e = find_slot(h, hash);
	int * v = hashvalue_ptr(e);
	if (v == NULL)
		return NULL;
	*n = (v == e->index) ? EMBED_HASH_SLOT : e->index[1];
	return v;
}

struct attrib_state *
attrib_newstate(const unsigned char *inherit_mask) {
	struct attrib_state *A = (struct attrib_state *)malloc(sizeof(*A));
	arena_init(&A->arena);
	hash_init(&A->arena_l);
	tuple_init(&A->tuple);
	tuple_hash_init(&A->tuple_l);
	inherit_cache_init(&A->icache);

	A->remove_head = 0;
	A->remove_tail = 0;
	A->kv_used = 0;
	A->kv_n = 0;

	if (inherit_mask == NULL) {
		memset(A->inherit_mask,1,sizeof(A->inherit_mask));
	} else {
		memcpy(A->inherit_mask, inherit_mask, sizeof(A->inherit_mask));
	}

	return A;
}

void
attrib_close(struct attrib_state *A) {
	arena_deinit(&A->arena);
	hash_deinit(&A->arena_l);
	tuple_deinit(&A->tuple);
	tuple_hash_deinit(&A->tuple_l);
	free(A);
}

int
attrib_entryid(struct attrib_state *A, int key, void *ptr, size_t sz) {
	uint32_t hash = kv_hash(key, ptr, sz);
	int n;
	int new_index;
	int *index = attrib_hash_lookup(&A->arena_l, hash, &n);
	if (index == NULL) {
		// new entry
		new_index = arena_create(&A->arena, key, ptr, sz, hash);
		attrib_hash_insert(&A->arena_l, hash, new_index);
	} else {
		int i;
		for (i=0;i<n && index[i]>=0;i++) {
			struct attrib_kv *kv = &A->arena.e[index[i]];
			if (kv->blob) {
				if (kv->v.ptr->sz == sz && memcmp(ptr, kv->v.ptr->data, sz) == 0) {
					return index[i];
				}
			} else if (sz <= EMBED_VALUE_SIZE && memcmp(ptr, kv->v.buffer, sz) == 0) {
				return index[i];
			}
		}
		new_index = arena_create(&A->arena, key, ptr, sz, hash);
		if (i==n) {
			attrib_hash_insert(&A->arena_l, hash, new_index);
		} else {
			index[i] = new_index;
			if (i+1 < n) {
				index[i+1] = -1;
			}
		}
	}
	A->kv_n++;
	return new_index;
}

static struct attrib_array *
create_attrib_array(int n, uint32_t hash) {
	size_t sz = sizeof(struct attrib_array) + n * sizeof(int) - sizeof(int);
	struct attrib_array * a = (struct attrib_array *)malloc(sz);
	a->refcount = 1;
	a->n = n;
	a->hash = hash;
	return a;
}

static void
kv_collect(struct attrib_state *A) {
	int i;
	for (i=0;i<A->arena.n;i++) {
		struct attrib_kv *kv = (struct attrib_kv *)&A->arena.e[i];
		if (kv->refcount == 0) {
			attrib_hash_remove(&A->arena_l, kv->hash, i);
			arena_release(&A->arena, i);
			--A->kv_n;
		}
	}
}


// add index(kv) into buffer[n]
static int
add_kv(struct attrib_state *A, int buffer[MAX_KEY], int n, int index) {
	struct attrib_kv *kv = &A->arena.e[index];
	int key = kv->k;
	int i;
	for (i=n-1;i>=0;i--) {
		int bk = A->arena.e[buffer[i]].k;
		if (key >= bk) {
			if (key > bk) {
				// insert index after [i]
				memmove(buffer+i+2, buffer+i+1,(n-i-1) * sizeof(int));
				buffer[i+1] = index;
				return 1;
			} else {
				// duplicate key
				buffer[i] = index;
				return 0;
			}
		}
	}
	memmove(buffer+1, buffer,n * sizeof(int));
	buffer[0] = index;
	return 1;
}

static inline attrib_t
addref(struct attrib_state *A, attrib_t handle) {
	A->tuple.s[handle.idx].a->refcount++;
	return handle;
}

attrib_t
attrib_addref(struct attrib_state *A, attrib_t handle) {
	return addref(A, handle);
}

attrib_t
attrib_create(struct attrib_state *A, int n, const int e[]) {
	int tmp[MAX_KEY];
	int i;
	if (n > 0) {
		tmp[0] = e[0];
		int index = 1;
		for (i=1;i<n;i++) {
			if (add_kv(A, tmp, index, e[i])) {
				++index;
			}
		}
		n = index;
	}
	uint32_t hash = array_hash(tmp, n);
	int index = tuple_hash_find(A, hash, n, tmp);
	if (index >= 0) {
		attrib_t ret = { index };
		return addref(A, ret);
	}

	struct attrib_array *a = create_attrib_array(n, hash);
	for (i=0;i<n;i++) {
		a->data[i] = tmp[i];
		if (arena_addref(&A->arena, tmp[i]) == 1) {
			A->kv_used++;
		}
	}
	if (A->kv_used * 2 < A->kv_n) {
		kv_collect(A);
	}
	int id = tuple_new(&A->tuple, a);
	tuple_hash_insert(&A->tuple_l, hash, id);
	attrib_t ret = { id };
	return ret;
}

static void
delete_tuple(struct attrib_state *A, int index) {
	struct attrib_array * a = A->tuple.s[index].a;
	if (a->refcount > 0) {
		// realive
		return;
	}
	int i;
	for (i=0;i<a->n;i++) {
		if (arena_release(&A->arena, a->data[i]) == 0) {
			A->kv_used--;
		}
	}
	tuple_hash_remove(&A->tuple_l, a->hash, index);
	tuple_delete(&A->tuple, index);

	inherit_cache_retirekey(&A->icache, index);
}

static int
pop_removed(struct attrib_state *A) {
	int r = A->removed[A->remove_head];
	int head = A->remove_head + 1;
	if (head >= DELAY_REMOVE)
		head -= DELAY_REMOVE;
	A->remove_head = head;
	return r;
}

int
attrib_release(struct attrib_state *A, attrib_t handle) {
	int index = handle.idx;
	assert(index >= 0 && index < A->tuple.n);
	struct attrib_array * a = A->tuple.s[index].a;
	--a->refcount;
	if (a->refcount == 0) {
		int tail = A->remove_tail;
		// push index, delay delete
		A->removed[tail] = index;
		if (++tail >= DELAY_REMOVE) {
			tail -= DELAY_REMOVE;
		}
		A->remove_tail = tail;
		if (tail == A->remove_head) {
			// full
			delete_tuple(A, pop_removed(A));
		}
	}
	return a->refcount;
}

static inline struct attrib_array *
get_array(struct attrib_state *A, attrib_t handle) {
	int index = handle.idx;
	assert(index >= 0 && index < A->tuple.n);
	struct attrib_array * a = A->tuple.s[index].a;
	return a;
}

int
attrib_get(struct attrib_state *A, attrib_t handle, int output[128]) {
	struct attrib_array *a = get_array(A, handle);
	memcpy(output, a->data, a->n * sizeof(int));
	return a->n;
}

static inline struct attrib_kv *
get_kv(struct attrib_state *A, struct attrib_array * a, int index) {
	return &A->arena.e[a->data[index]];
}

int
attrib_find(struct attrib_state *A, attrib_t handle, uint8_t key) {
	int index = handle.idx;
	assert(index >= 0 && index < A->tuple.n);
	struct attrib_array * a = A->tuple.s[index].a;
	int begin = 0;
	int end = a->n;
	while (begin < end) {
		int mid = (begin + end) / 2;
		struct attrib_kv *kv = get_kv(A, a, mid);
		if (kv->k == key) {
			return mid;
		} else if (kv->k < key) {
			begin = mid + 1;
		} else {
			end = mid;
		}
	}
	return -1;
}

void*
attrib_index(struct attrib_state *A, attrib_t handle, int i, uint8_t *key) {
	int index = handle.idx;
	assert(index >= 0 && index < A->tuple.n);
	struct attrib_array * a = A->tuple.s[index].a;
	if (i < 0 || i >= a->n)
		return NULL;
	struct attrib_kv *kv = get_kv(A, a, i);
	*key = kv->k;
	return kv->blob ? kv->v.ptr->data : kv->v.buffer;
}

static attrib_t
attrib_inherit_(struct attrib_state *A, attrib_t child, attrib_t parent, int with_mask) {
	struct attrib_array *child_a = get_array(A, child);
	struct attrib_array *parent_a = get_array(A, parent);
	int output[MAX_KEY];
	int output_index = 0;
	int dirty = 0;
	const unsigned char * inherit_mask = A->inherit_mask;
	if (child_a->n == 0) {
		// child is empty
		if (with_mask) {
			int i;
			for (i=0;i<parent_a->n;i++) {
				struct attrib_kv *kv = get_kv(A, parent_a, i);
				if (inherit_mask[kv->k]) {
					output[output_index++] = parent_a->data[i];
				} else {
					dirty = 1;
				}
			}
			if (dirty) {
				return attrib_create(A, output_index, output);
			} else {
				addref(A, parent);
				return parent;
			}
		} else {
			addref(A, parent);
			return parent;
		}
	}
	int child_index = 0;
	int parent_index = 0;
	for (;;) {
		if (parent_index >= parent_a->n) {
			int n = child_a->n - child_index;
			memcpy(output+output_index, child_a->data+child_index, n * sizeof(int));
			output_index += n;
			break;
		}
		if (child_index >= child_a->n) {
			int n = parent_a->n - parent_index;
			if (with_mask) {
				int i;
				for (i=0;i<n;i++) {
					struct attrib_kv *kv = get_kv(A, parent_a, parent_index+i);
					if (inherit_mask[kv->k]) {
						output[output_index++] = parent_a->data[parent_index+i];
						dirty = 1;
					}
				}
			} else {
				dirty = 1;
				memcpy(output+output_index, parent_a->data+parent_index, n * sizeof(int));
				output_index += n;
			}
			break;
		}
		int child_v = child_a->data[child_index];
		int parent_v = parent_a->data[parent_index];

		struct attrib_kv *child_kv = get_kv(A, child_a, child_index);
		struct attrib_kv *parent_kv = get_kv(A, parent_a, parent_index);
		int child_k = child_kv->k;
		int parent_k = parent_kv->k;

		if (child_k == parent_k) {
			// ignore parent
			++parent_index;
			++child_index;
			output[output_index++] = child_v;
		} else if (child_k < parent_k) {
			// use child
			++child_index;
			output[output_index++] = child_v;
		} else {
			// use parent
			if (with_mask) {
				if (inherit_mask[parent_k]) {
					output[output_index++] = parent_v;
					dirty = 1;
				}
			} else {
				output[output_index++] = parent_v;
				dirty = 1;
			}
			++parent_index;
		}
	}
	if (dirty) {
		return attrib_create(A, output_index, output);
	} else {
		addref(A, child);
		return child;
	}
}

attrib_t
attrib_inherit(struct attrib_state *A, attrib_t child, attrib_t parent, int withmask) {
	// check cache
	int result = inherit_cache_fetch(&A->icache, child.idx, parent.idx, withmask);
	if (result >= 0) {
		attrib_t r = { result };
		addref(A, r);
		return r;
	}
	attrib_t r = attrib_inherit_(A, child, parent, withmask);
	inherit_cache_set(&A->icache, child.idx, parent.idx, withmask, r.idx);
	return r;
}

int
attrib_refcount(struct attrib_state *A, attrib_t attr) {
	int index = attr.idx;
	struct attrib_array * a = A->tuple.s[index].a;
	return a->refcount;
}

#ifdef ATTRIB_TEST_MAIN

#include <stdio.h>

#define KV(A, key, s) attrib_entryid(A, key, s "", sizeof(s))

static void
dump_attrib(struct attrib_state *A, attrib_t handle) {
	int i;
	printf("[ATTRIB %x (%d)]\n", handle.idx, attrib_refcount(A, handle));
	for (i=0;;i++) {
		uint8_t key;
		void *ptr = attrib_index(A, handle, i, &key);
		if (ptr == NULL)
			break;
		printf("\t[%d] = %s\n", key, (const char *)ptr);
	}
}

int
main() {
	struct attrib_state *A = attrib_newstate(NULL);
	int id1 = KV(A, 1, "hello");
	int id2 = KV(A, 2, "hello world");
	int id3 = KV(A, 2, "hello");
	int id4 = KV(A, 1, "hello");
	int id5 = KV(A, 2, "hello world");

	assert(id1 == id4);
	assert(id2 == id5);
	assert(id1 != id3);

	int tuple[] = { id2, id3, id1 };

	attrib_t handle = attrib_create(A, 3, tuple);

	int tmp[128];
	int n = attrib_get(A, handle, tmp);
	int key_index = attrib_find(A, handle, 1);
	tmp[key_index] = KV(A, 1, "world");

	attrib_t handle2 = attrib_create(A, n, tmp);
	attrib_t handle3 = attrib_create(A, n, tmp);
	assert(handle2.idx == handle3.idx);

	dump_attrib(A, handle);
	dump_attrib(A, handle2);

	printf("mem = %d\n", (int)attrib_memsize(A));

	attrib_t handle4 = attrib_inherit(A, handle, handle2, 0);
	attrib_release(A, handle);
	attrib_release(A, handle2);
	dump_attrib(A, handle4);

	attrib_release(A, handle4);

	dump_attrib(A, handle4);

	handle4 = attrib_inherit(A, handle4, handle4, 0);

	dump_attrib(A, handle4);

	attrib_close(A);
	return 0;
}

#endif