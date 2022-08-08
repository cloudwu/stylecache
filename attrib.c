#include "attrib.h"
#include "hash.h"
#include "inherit_cache.h"
#include "intern_cache.h"
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define DEFAULT_ATTRIB_ARENA_BITS 7
#define DEFAULT_ATTRIB_ARENA_SIZE (1 << DEFAULT_ATTRIB_ARENA_BITS)
#define DEFAULT_TUPLE_BITS 7
#define DEFAULT_TUPLE_SIZE (1 << DEFAULT_TUPLE_BITS)
#define EMBED_VALUE_SIZE 8
#define MAX_KEY 128
#define DELAY_REMOVE 4096


// #define VERIFY_ATTRIBID

#ifdef VERIFY_ATTRIBID

#define MAX_ATTRIBID 0x10000

struct verify_attrib {
	int n;
	int lastid;
	struct {
		int vid;
		int id;
	} id[MAX_ATTRIBID];
};

#define VERIFY_ATTRIB struct verify_attrib v;

#else

#define VERIFY_ATTRIB

#endif

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

struct delay_removed {
	int head;
	int tail;
	int removed[DELAY_REMOVE];
};

struct attrib_state {
	struct attrib_arena arena;
	struct intern_cache arena_i;
	struct attrib_tuple tuple;
	struct intern_cache tuple_i;
	struct inherit_cache icache;
	struct delay_removed kv_removed;
	struct delay_removed tuple_removed;
	unsigned char inherit_mask[128];
	VERIFY_ATTRIB
};


#ifdef VERIFY_ATTRIBID

static inline void
verify_attrib_init(struct attrib_state *A) {
	struct verify_attrib *v = &A->v;
	v->lastid = 0;
	v->n = 0;
}

static inline attrib_t
verify_attrib_alloc(struct attrib_state *A, int id) {
	struct verify_attrib *v = &A->v;
	int vid = ++v->lastid;
	assert(vid != 0);
	int n = v->n++;
	assert(n < MAX_ATTRIBID);
	v->id[n].vid = vid;
	v->id[n].id = id;
	attrib_t r = { vid };
	return r;
}

static inline int
verify_attrib_find_(struct verify_attrib *v, int vid) {
	int begin = 0;
	int end = v->n;
	while (begin < end) {
		int mid = (begin + end)/2;
		int midv = v->id[mid].vid;
		if (midv == vid) {
			return mid;
		} else if (midv < vid) {
			begin = mid + 1;
		} else {
			end = mid;
		}
	}
	assert(0);
	return -1;
}

static inline void
verify_attrib_dealloc(struct attrib_state *A, int vid) {
	struct verify_attrib *v = &A->v;
	int p = verify_attrib_find_(v, vid);
	--v->n;
	memmove(v->id + p, v->id + p + 1, (v->n - p) * sizeof(v->id[0]));
}

static inline int
verify_attribid(struct attrib_state *A, int vid) {
	struct verify_attrib *v = &A->v;
	int p = verify_attrib_find_(v, vid);
	return v->id[p].id;
}

#else

static inline void verify_attrib_init(struct attrib_state *A) {}
static inline attrib_t verify_attrib_alloc(struct attrib_state *A, int id) { attrib_t r = { id }; return r; }
static inline void verify_attrib_dealloc(struct attrib_state *A, int id) {}
static inline int verify_attribid(struct attrib_state *A, int id) { return id; }

#endif


static int
delay_remove(struct delay_removed *removed, int index) {
	int tail = removed->tail;
	// push index, delay delete
	removed->removed[tail] = index;
	if (++tail >= DELAY_REMOVE) {
		tail -= DELAY_REMOVE;
	}
	removed->tail = tail;
	if (tail == removed->head) {
		// delay queue is full
		int removed_index = removed->removed[removed->head];
		int head = removed->head + 1;
		if (head >= DELAY_REMOVE)
			head -= DELAY_REMOVE;
		removed->head = head;
		return removed_index;
	}
	return -1;
}

static void
delay_remove_init(struct delay_removed *r) {
	r->head = 0;
	r->tail = 0;
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
tuple_size(struct attrib_tuple *tuple) {
	size_t sz = tuple->cap * sizeof(union attrib_tuple_entry);
	return sz;
}

size_t
attrib_memsize(struct attrib_state *A) {
	size_t sz = sizeof(*A);
	sz += arena_size(&A->arena);
	sz += tuple_size(&A->tuple);
	sz += intern_cache_memsize(&A->arena_i);
	sz += intern_cache_memsize(&A->tuple_i);
	return sz;
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

struct attrib_state *
attrib_newstate(const unsigned char inherit_mask[128]) {
	struct attrib_state *A = (struct attrib_state *)malloc(sizeof(*A));
	arena_init(&A->arena);
	tuple_init(&A->tuple);
	inherit_cache_init(&A->icache);
	delay_remove_init(&A->kv_removed);
	delay_remove_init(&A->tuple_removed);
	intern_cache_init(&A->arena_i, DEFAULT_ATTRIB_ARENA_BITS);
	intern_cache_init(&A->tuple_i, DEFAULT_TUPLE_BITS);
	verify_attrib_init(A);

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
	tuple_deinit(&A->tuple);
	inherit_cache_deinit(&A->icache);
	intern_cache_deinit(&A->arena_i);
	intern_cache_deinit(&A->tuple_i);
	free(A);
}

static uint32_t
attrib_kv_hash_(uint32_t index, void *a) {
	struct attrib_kv *e = (struct attrib_kv *)a;
	return e[index].hash;
}

#define ATTRIB_KV_HASH(A) attrib_kv_hash_, A->arena.e

int
attrib_entryid(struct attrib_state *A, int key, void *ptr, size_t sz) {
	uint32_t hash = kv_hash(key, ptr, sz);
	struct intern_cache_iterator iter;
	if (intern_cache_find(&A->arena_i, hash, &iter, ATTRIB_KV_HASH(A))) {
		do {
			struct attrib_kv *kv = &A->arena.e[iter.result];
			if (kv->k == key) {
				if (kv->blob) {
					if (kv->v.ptr->sz == sz && memcmp(ptr, kv->v.ptr->data, sz) == 0) {
						return iter.result;
					}
				} else {
					if (sz <= EMBED_VALUE_SIZE && memcmp(ptr, kv->v.buffer, sz) == 0) {
						return iter.result;
					}
				}
			}
		} while (intern_cache_find_next(&A->arena_i, &iter,  ATTRIB_KV_HASH(A)));
	}
	// new entry
	int	new_index = arena_create(&A->arena, key, ptr, sz, hash);
	intern_cache_insert(&A->arena_i, new_index, ATTRIB_KV_HASH(A));
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
release_kv(struct attrib_state *A, int removed_index) {
	struct attrib_arena *arena = &(A->arena);
	struct attrib_kv * kv = &arena->e[removed_index];
	if (--kv->refcount == 0) {
		intern_cache_remove(&A->arena_i, removed_index,  ATTRIB_KV_HASH(A));
		if (kv->blob) {
			free(kv->v.ptr);
			kv->blob = 0;
		}
		kv->v.next = arena->freelist;
		arena->freelist = removed_index;
	}
}

static int
arena_release(struct attrib_state *A, int id) {
	struct attrib_arena *arena = &(A->arena);
	assert(id >= 0 && id < arena->n);
	struct attrib_kv * kv = &arena->e[id];
	assert(kv->refcount > 0);
	int c = --kv->refcount;
	if (c == 0) {
		kv->refcount = 1;
		int removed_index = delay_remove(&A->kv_removed, id);
		if (removed_index >= 0) {
			release_kv(A, removed_index);
		}
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
	int index = verify_attribid(A, handle.idx);
	A->tuple.s[index].a->refcount++;
	return handle;
}

attrib_t
attrib_addref(struct attrib_state *A, attrib_t handle) {
	return addref(A, handle);
}

#ifdef VERIFY_ATTRIBID

static uint32_t
tuple_hash_(uint32_t index, void *t) {
	struct attrib_state *A = (struct attrib_state *)t;
	union attrib_tuple_entry *e = A->tuple.s;
	return e[verify_attribid(A, index)].a->hash;
}

#define TUPLE_HASH(A) tuple_hash_, A

#else

static uint32_t
tuple_hash_(uint32_t index, void *t) {
	union attrib_tuple_entry *e = (union attrib_tuple_entry *)t;
	return e[index].a->hash;
}

#define TUPLE_HASH(A) tuple_hash_, A->tuple.s

#endif

static int
tuple_hash_find(struct attrib_state *A, uint32_t hash, int n, int *buf) {
	struct intern_cache_iterator iter;
	if (intern_cache_find(&A->tuple_i, hash, &iter, TUPLE_HASH(A))) {
		do {
			struct attrib_array *a = A->tuple.s[verify_attribid(A, iter.result)].a;
			if (n == a->n && memcmp(buf, a->data, n * sizeof(int)) == 0) {
				return iter.result;
			}
		} while (intern_cache_find_next(&A->tuple_i, &iter, TUPLE_HASH(A)));
	}
	return -1;
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
		arena_addref(&A->arena, tmp[i]);
	}
	int id = tuple_new(&A->tuple, a);

	attrib_t ret = verify_attrib_alloc(A, id);

	intern_cache_insert(&A->tuple_i, ret.idx, TUPLE_HASH(A));

	return ret;
}

static void
delete_tuple(struct attrib_state *A, int index) {
	struct attrib_array * a = A->tuple.s[index].a;
	if (--a->refcount > 0)
		return;
	int i;
	for (i=0;i<a->n;i++) {
		arena_release(A, a->data[i]);
	}
	intern_cache_remove(&A->tuple_i, index, TUPLE_HASH(A));
	tuple_delete(&A->tuple, index);

	inherit_cache_retirekey(&A->icache, index);
}

int
attrib_release(struct attrib_state *A, attrib_t handle) {
	int index = verify_attribid(A, handle.idx);
	assert(index >= 0 && index < A->tuple.n);
	struct attrib_array * a = A->tuple.s[index].a;
	--a->refcount;
	if (a->refcount == 0) {
		a->refcount = 1;	// keep ref in delay queue
		int removed_index = delay_remove(&A->tuple_removed, handle.idx);
		if (removed_index >= 0) {
			delete_tuple(A, verify_attribid(A, removed_index));
			verify_attrib_dealloc(A, removed_index);
		}
	}
	return a->refcount;
}

static inline struct attrib_array *
get_array(struct attrib_state *A, attrib_t handle) {
	int index = verify_attribid(A, handle.idx);
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
	int index = verify_attribid(A, handle.idx);
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
	int index = verify_attribid(A, handle.idx);
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
	int index = verify_attribid(A, attr.idx);
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