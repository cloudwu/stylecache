#include "attrib.h"
#include "hash.h"
#include "inherit_cache.h"
#include "intern_cache.h"
#include <stdint.h>
#include <assert.h>
#include <string.h>
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
	v->lastid = 100000;
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

static void
tuple_init(struct attrib_tuple *tuple, struct style_cache *C) {
	tuple->n = 0;
	tuple->cap = DEFAULT_TUPLE_SIZE;
	tuple->s = (union attrib_tuple_entry *)style_malloc(C, DEFAULT_TUPLE_SIZE * sizeof(union attrib_tuple_entry));
	tuple->freelist = -1;
}

static inline size_t
attrib_array_size(int n) {
	return sizeof(struct attrib_array) + n * sizeof(int) - sizeof(int);
}

static void
clear_freelist(struct attrib_tuple *tuple) {
	int index = tuple->freelist;
	while (index >= 0) {
		int next = tuple->s[index].next;
		tuple->s[index].a = NULL;
		index = next;
	}
}

static void
tuple_deinit(struct attrib_tuple *tuple, struct style_cache *C) {
	int i;
	clear_freelist(tuple);
	for (i=0;i<tuple->n;i++) {
		struct attrib_array *a = tuple->s[i].a;
		if (a) {
			style_free(C, a, attrib_array_size(a->n));
		}
	}
	style_free(C, tuple->s, tuple->cap * sizeof(union attrib_tuple_entry));
}

static int
tuple_new(struct attrib_tuple *tuple, struct attrib_array *data, struct style_cache *C) {
	int index = tuple->freelist;
	if (index >= 0) {
		tuple->freelist = tuple->s[index].next;
	} else {
		index = tuple->n++;
		if (index >= tuple->cap) {
			int newcap = tuple->cap * 3 / 2;
			tuple->s = (union attrib_tuple_entry *)style_realloc(C, tuple->s, tuple->cap * sizeof(union attrib_tuple_entry), newcap * sizeof(union attrib_tuple_entry));
			assert(tuple->s != NULL);
			tuple->cap = newcap;
		}
	}
	tuple->s[index].a = data;
	return index;
}

static void
tuple_delete(struct attrib_tuple *tuple, int index, struct style_cache *C) {
	assert(index >=0 && index < tuple->n);
	struct attrib_array *a = tuple->s[index].a;
	style_free(C, a, attrib_array_size(a->n));
	tuple->s[index].a = NULL;
	tuple->s[index].next = tuple->freelist;
	tuple->freelist = index;
}

static void
arena_init(struct attrib_arena *arena, struct style_cache *C) {
	arena->e = (struct attrib_kv *)style_malloc(C, DEFAULT_ATTRIB_ARENA_SIZE * sizeof(struct attrib_kv));
	arena->n = 0;
	arena->cap = DEFAULT_ATTRIB_ARENA_SIZE;
	arena->freelist = -1;
}

static inline void
free_blob(struct attrib_kv *kv, struct style_cache *C) {
	if (kv->blob) {
		style_free(C, kv->v.ptr, kv->v.ptr->sz + sizeof(struct attrib_blob) - 1);
		kv->blob = 0;
	}
}

static void
arena_deinit(struct attrib_arena *arena, struct style_cache *C) {
	int i;
	for (i=0;i<arena->n;i++) {
		free_blob(&arena->e[i], C);
	}
	style_free(C, arena->e, arena->cap * sizeof(struct attrib_kv));
}

static struct attrib_blob *
blob_new(void *ptr, size_t sz, struct style_cache *C) {
	struct attrib_blob * b = (struct attrib_blob *)style_malloc(C, sizeof(*b) - 1 + sz);
	b->sz = sz;
	memcpy(b->data, ptr, sz);
	return b;
}

static int
arena_create(struct attrib_arena *arena, int key, void *value, size_t sz, uint32_t hash, struct style_cache *C) {
	struct attrib_kv *kv;
	int index;
	if (arena->freelist >= 0) {
		index = arena->freelist;
		kv = &arena->e[index];
		arena->freelist = kv->v.next;
	} else {
		if (arena->n >= arena->cap) {
			int newcap = arena->cap * 3 / 2;
			arena->e = (struct attrib_kv *)style_realloc(C, arena->e, arena->cap * sizeof(struct attrib_kv), newcap * sizeof(struct attrib_kv));
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
		kv->v.ptr = blob_new(value, sz, C);
	} else {
		kv->blob = 0;
		kv->v.ptr = NULL;
		memcpy(kv->v.buffer, value, sz);
	}
	return index;
}

struct attrib_state *
attrib_newstate(const unsigned char inherit_mask[128], struct style_cache *C) {
	struct attrib_state *A = (struct attrib_state *)style_malloc(C, sizeof(*A));
	arena_init(&A->arena, C);
	tuple_init(&A->tuple, C);
	inherit_cache_init(&A->icache);
	delay_remove_init(&A->kv_removed);
	delay_remove_init(&A->tuple_removed);
	intern_cache_init(C, &A->arena_i, DEFAULT_ATTRIB_ARENA_BITS);
	intern_cache_init(C, &A->tuple_i, DEFAULT_TUPLE_BITS);
	verify_attrib_init(A);

	if (inherit_mask == NULL) {
		memset(A->inherit_mask,1,sizeof(A->inherit_mask));
	} else {
		memcpy(A->inherit_mask, inherit_mask, sizeof(A->inherit_mask));
	}

	return A;
}

void
attrib_close(struct attrib_state *A, struct style_cache *C) {
	arena_deinit(&A->arena, C);
	tuple_deinit(&A->tuple, C);
	inherit_cache_deinit(C, &A->icache);
	intern_cache_deinit(C, &A->arena_i);
	intern_cache_deinit(C, &A->tuple_i);
	style_free(C, A, sizeof(*A));
}

static uint32_t
attrib_kv_hash_(uint32_t index, void *a) {
	struct attrib_kv *e = (struct attrib_kv *)a;
	return e[index].hash;
}

#define ATTRIB_KV_HASH(A) attrib_kv_hash_, A->arena.e

int
attrib_entryid(struct attrib_state *A, int key, void *ptr, size_t sz, struct style_cache *C) {
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
	int	new_index = arena_create(&A->arena, key, ptr, sz, hash, C);
	intern_cache_insert(&A->arena_i, new_index, ATTRIB_KV_HASH(A), C);
	return new_index;
}

static struct attrib_array *
create_attrib_array(int n, uint32_t hash, struct style_cache *C) {
	struct attrib_array * a = (struct attrib_array *)style_malloc(C, attrib_array_size(n));
	a->refcount = 1;
	a->n = n;
	a->hash = hash;
	return a;
}

static void
release_kv(struct attrib_state *A, int removed_index, struct style_cache *C) {
	struct attrib_arena *arena = &(A->arena);
	struct attrib_kv * kv = &arena->e[removed_index];
	if (--kv->refcount == 0) {
		intern_cache_remove(&A->arena_i, removed_index,  ATTRIB_KV_HASH(A));
		free_blob(kv, C);
		kv->v.next = arena->freelist;
		arena->freelist = removed_index;
	}
}

void
attrib_entry_release(struct attrib_state *A, int id, struct style_cache *C) {
	struct attrib_arena *arena = &(A->arena);
	assert(id >= 0 && id < arena->n);
	struct attrib_kv * kv = &arena->e[id];
	assert(kv->refcount > 0);
	int c = --kv->refcount;
	if (c == 0) {
		kv->refcount = 1;
		int removed_index = delay_remove(&A->kv_removed, id);
		if (removed_index >= 0) {
			release_kv(A, removed_index, C);
		}
	}
}

void
attrib_entry_addref(struct attrib_state *A, int id) {
	struct attrib_arena *arena = &(A->arena);
	assert(id >= 0 && id < arena->n);
	struct attrib_kv * kv = &arena->e[id];
	++kv->refcount;
	assert(kv->refcount != 0);
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
attrib_create(struct attrib_state *A, int n, const int e[], struct style_cache *C) {
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

	struct attrib_array *a = create_attrib_array(n, hash, C);
	for (i=0;i<n;i++) {
		a->data[i] = tmp[i];
		attrib_entry_addref(A, tmp[i]);
	}
	int id = tuple_new(&A->tuple, a, C);

	attrib_t ret = verify_attrib_alloc(A, id);

	intern_cache_insert(&A->tuple_i, ret.idx, TUPLE_HASH(A), C);

	return ret;
}

static int
delete_tuple(struct attrib_state *A, int index, struct style_cache *C) {
	int id = verify_attribid(A, index);
	struct attrib_array * a = A->tuple.s[id].a;
	if (--a->refcount > 0)
		return 0;
	int i;
	for (i=0;i<a->n;i++) {
		attrib_entry_release(A, a->data[i], C);
	}
	intern_cache_remove(&A->tuple_i, index, TUPLE_HASH(A));
	tuple_delete(&A->tuple, id, C);

	inherit_cache_retirekey(&A->icache, index);
	return 1;
}

int
attrib_release(struct attrib_state *A, attrib_t handle, struct style_cache *C) {
	int index = verify_attribid(A, handle.idx);
	assert(index >= 0 && index < A->tuple.n);
	struct attrib_array * a = A->tuple.s[index].a;
	--a->refcount;
	if (a->refcount == 0) {
		a->refcount = 1;	// keep ref in delay queue
		int removed_index = delay_remove(&A->tuple_removed, handle.idx);
		if (removed_index >= 0) {
			if (delete_tuple(A, removed_index, C))
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
attrib_entry_get(struct attrib_state *A, int index, uint8_t *key, size_t *sz) {
	struct attrib_kv *kv = &A->arena.e[index];
	*key = kv->k;
	if (sz) {
		*sz = kv->blob ? kv->v.ptr->sz : EMBED_VALUE_SIZE;
	}
	return kv->blob ? kv->v.ptr->data : kv->v.buffer;
}

int
attrib_index(struct attrib_state *A, attrib_t handle, int i) {
	int index = verify_attribid(A, handle.idx);
	assert(index >= 0 && index < A->tuple.n);
	struct attrib_array * a = A->tuple.s[index].a;
	if (i < 0 || i >= a->n)
		return -1;
	return a->data[i];
}

static attrib_t
attrib_inherit_(struct attrib_state *A, attrib_t child, attrib_t parent, int with_mask, struct style_cache *C) {
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
				return attrib_create(A, output_index, output, C);
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
		return attrib_create(A, output_index, output, C);
	} else {
		addref(A, child);
		return child;
	}
}

attrib_t
attrib_inherit(struct attrib_state *A, attrib_t child, attrib_t parent, int withmask, struct style_cache *C) {
	// check cache
	int result = inherit_cache_fetch(&A->icache, child.idx, parent.idx, withmask);
	if (result >= 0) {
		attrib_t r = { result };
		addref(A, r);
		return r;
	}
	attrib_t r = attrib_inherit_(A, child, parent, withmask, C);
	inherit_cache_set(&A->icache, child.idx, parent.idx, withmask, r.idx, C);
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
#include "style.h"

#define KV(A, key, s) attrib_entryid(A, key, s "", sizeof(s), C)

static void
dump_attrib(struct attrib_state *A, attrib_t handle) {
	int i;
	printf("[ATTRIB %x (%d)]\n", handle.idx, attrib_refcount(A, handle));
	for (i=0;;i++) {
		int id = attrib_index(A, handle, i);
		if (id < 0)
			break;
		uint8_t key;
		size_t sz;
		void* ptr = attrib_entry_get(A, id, &key, &sz);
		printf("\t[%d] = %s\n", key, (const char *)ptr);
	}
}

int
main() {
	struct style_cache *C = style_newcache(NULL, NULL, NULL);
	struct attrib_state *A = attrib_newstate(NULL, C);
	int id1 = KV(A, 1, "hello");
	int id2 = KV(A, 2, "hello world");
	int id3 = KV(A, 2, "hello");
	int id4 = KV(A, 1, "hello");
	int id5 = KV(A, 2, "hello world");

	assert(id1 == id4);
	assert(id2 == id5);
	assert(id1 != id3);

	int tuple[] = { id2, id3, id1 };

	attrib_t handle = attrib_create(A, 3, tuple, C);

	int tmp[128];
	int n = attrib_get(A, handle, tmp);
	int key_index = attrib_find(A, handle, 1);
	tmp[key_index] = KV(A, 1, "world");

	attrib_t handle2 = attrib_create(A, n, tmp, C);
	attrib_t handle3 = attrib_create(A, n, tmp, C);
	assert(handle2.idx == handle3.idx);

	dump_attrib(A, handle);
	dump_attrib(A, handle2);

	attrib_t handle4 = attrib_inherit(A, handle, handle2, 0, C);
	attrib_release(A, handle, C);
	attrib_release(A, handle2, C);
	dump_attrib(A, handle4);

	attrib_release(A, handle4, C);

	dump_attrib(A, handle4);

	handle4 = attrib_inherit(A, handle4, handle4, 0, C);

	dump_attrib(A, handle4);

	attrib_close(A, C);

	style_deletecache(C);
	return 0;
}

#endif