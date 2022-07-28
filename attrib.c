#include "attrib.h"
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
#define HASH_SIZE 128
#define HASH_MAXSTEP 5
#define MAX_KEY 128

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
	struct attrib_tuple_hash_entry *e;
};

struct attrib_state {
	struct attrib_arena arena;
	struct attrib_lookup arena_l;
	struct attrib_tuple tuple;
	struct attrib_tuple_lookup tuple_l;
	int kv_used;
	int kv_n;
};

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
		printf("COPY %s %d\n", (const char *)value, (int)sz);
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
init_slots(struct attrib_lookup *h, int n) {
	h->e = (struct attrib_hash_entry *)malloc(n * sizeof(struct attrib_hash_entry));
	h->n = n;
	int i;
	for (i=0;i<n;i++) {
		// set invalid
		h->e[i].index[0] = -1;
		h->e[i].index[1] = -1;
	}
}

static void
hash_init(struct attrib_lookup *h) {
	init_slots(h, HASH_SIZE);
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
init_tuple_lut_slots(struct attrib_tuple_lookup *lut, int n) {
	lut->e = (struct attrib_tuple_hash_entry *)malloc(n * sizeof(struct attrib_tuple_hash_entry));
	lut->n = n;
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
	int slot = hash & (h->n - 1);
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
	int slot = hash & (h->n - 1);
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

	init_tuple_lut_slots(h, n * 2);
	for (i=0;i<n;i++) {
		struct attrib_tuple_hash_entry *e = &h->e[i];
		if (e->hash != 0 && e->index >=0) {
			tuple_hash_insert(h, e->hash, e->index);
		}
	}
	free(e);
}

static void
tuple_hash_remove(struct attrib_tuple_lookup *h, uint32_t hash, int index) {
	int slot = hash & (h->n - 1);
	for (;;) {
		struct attrib_tuple_hash_entry *e = &h->e[slot];
		if (e->hash == hash && e->index == index) {
			e->index = -1;
			return;
		}
		slot += HASH_STEP;
		if (slot >= h->n)
			slot -= h->n;
	}
}

static void
tuple_hash_init(struct attrib_tuple_lookup *lut) {
	init_tuple_lut_slots(lut, HASH_SIZE);
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
	init_slots(h, n * 2);
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
	int index = hash & (h->n - 1);
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
	int mainslot = hash & (h->n - 1);
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

static uint32_t
kv_hash(int key, void *value, size_t l) {
	uint8_t *str = (uint8_t *)value;
	uint32_t h = (uint32_t)(key ^ l);
	for (; l > 0; l--)
		h ^= ((h<<5) + (h>>2) + (str[l - 1]));
	return h;
}

static uint32_t
array_hash(int *v, int n) {
	uint8_t *str = (uint8_t*)v;
	uint32_t h = (uint32_t)(n);
	int l = n*4;
	for (; l > 0; l--)
		h ^= ((h<<5) + (h>>2) + (str[l - 1]));
	// hash 0 is reserved for empty slot
	if (h == 0)
		return 1;
	return h;
}

struct attrib_state *
attrib_newstate() {
	struct attrib_state *A = (struct attrib_state *)malloc(sizeof(*A));
	arena_init(&A->arena);
	hash_init(&A->arena_l);
	tuple_init(&A->tuple);
	tuple_hash_init(&A->tuple_l);

	A->kv_used = 0;
	A->kv_n = 0;

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


static void
add_kv(struct attrib_state *A, int buffer[MAX_KEY], int n, int index) {
	struct attrib_kv *kv = &A->arena.e[index];
	int key = kv->k;
	int i;
	for (i=n-1;i>=0;i--) {
		int bk = A->arena.e[buffer[i]].k;
		if (key >= bk) {
			assert(key > bk);
			// insert index after [i]
			memmove(buffer+i+2, buffer+i+1,(n-i-1) * sizeof(int));
			buffer[i+1] = index;
			return;
		}
	}
	memmove(buffer+1, buffer,n * sizeof(int));
	buffer[0] = index;	
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
	for (i=0;i<n;i++) {
		add_kv(A, tmp, i, e[i]);
	}
	uint32_t hash = array_hash(tmp, n);

	int index = tuple_hash_find(A, hash, n, e);
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

void
attrib_release(struct attrib_state *A, attrib_t handle) {
	int index = handle.idx;
	assert(index >= 0 && index < A->tuple.n);
	struct attrib_array * a = A->tuple.s[index].a;
	--a->refcount;
	if (a->refcount == 0) {
		int i;
		for (i=0;i<a->n;i++) {
			if (arena_release(&A->arena, a->data[i]) == 0) {
				A->kv_used--;
			}
		}
		tuple_hash_remove(&A->tuple_l, a->hash, index);
		tuple_delete(&A->tuple, index);
	}
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

attrib_t
attrib_inherit(struct attrib_state *A, attrib_t child, attrib_t parent, const unsigned char * inherit_mask) {
	struct attrib_array *child_a = get_array(A, child);
	struct attrib_array *parent_a = get_array(A, parent);
	int output[MAX_KEY];
	int output_index = 0;
	int dirty = 0;
	if (child_a->n == 0) {
		// child is empty
		if (inherit_mask) {
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
			if (inherit_mask) {
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
				break;
			}
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
			if (inherit_mask) {
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
	for (i=0;;i++) {
		int key;
		void *ptr = attrib_index(A, handle, i, &key);
		if (ptr == NULL)
			break;
		printf("[%d] = %s\n", key, (const char *)ptr);
	}
}

int
main() {
	struct attrib_state *A = attrib_newstate();
	int id1 = KV(A, 1, "hello");
	int id2 = KV(A, 2, "hello world");
	int id3 = KV(A, 2, "hello");
	int id4 = KV(A, 1, "hello");
	int id5 = KV(A, 2, "hello world");

	assert(id1 == id4);
	assert(id2 == id5);
	assert(id1 != id3);

	int tuple[] = { id2, id1 };

	attrib_t handle = attrib_create(A, 2, tuple);

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

	attrib_t handle4 = attrib_inherit(A, handle, handle2, NULL);
	dump_attrib(A, handle4);

	attrib_release(A, handle4);

	attrib_close(A);
	return 0;
}

#endif