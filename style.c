#include "style.h"
#include "attrib.h"
#include "style_alloc.h"
#include "dirtylist.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#define INVALID_NODE (~0)
#define ARENA_DEFAULT_SIZE 1024
#define ATTRIB_CB_NODE 1024
#define ATTRIB_CB_SIZE 64

#define MAX_KEY 128

// Every styles created in current frame are linked in .prev/.next
// freelist linked in .next
struct style {
	int a;
	int b;
	attrib_t value;
	int prev;
	int next;
	int refcount:31;
	int withmask:1;
};

struct blob {
	size_t sz;
	void *ptr;
};

struct attrib_cb {
	char data[ATTRIB_CB_NODE][ATTRIB_CB_SIZE];
	int small_n;
	int cap;
	int n;
	struct blob *big;
};

struct style_cache {
	style_alloc alloc;
	void * alloc_ud;
	struct attrib_state *A;
	struct style *s;
	struct dirtylist *D;
	struct attrib_cb free_attrib;
	style_handle_t empty;
	int n;
	int cap;
	int freelist;
	int live;
	int dead;
	unsigned char mask[MAX_KEY];
};

static void *
default_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
	if (nsize == 0) {
		free(ptr);
		return NULL;
	} else {
		return realloc(ptr, nsize);
	}
}

void *
style_malloc(struct style_cache *c, size_t size) {
	return c->alloc(c->alloc_ud, NULL, 0, size);
}

void
style_free(struct style_cache *c, void *ptr, size_t osize) {
	c->alloc(c->alloc_ud, ptr, osize, 0);
}

void *
style_realloc(struct style_cache *c, void *ptr, size_t osize, size_t nsize) {
	return c->alloc(c->alloc_ud, ptr, osize, nsize);
}

static void
attrib_cb_init(struct attrib_cb *A) {
	A->small_n = 0;
	A->cap = 0;
	A->n = 0;
	A->big = NULL;
}

static void
attrib_cb_deinit(struct attrib_cb *A, struct style_cache *C) {
	if (A->big) {
		int i;
		for (i=0;i<A->n;i++) {
			style_free(C, A->big[i].ptr, A->big[i].sz);
		}
		style_free(C, A->big, A->cap * sizeof(*A->big));
	}
}

static void
attrib_cb_add(struct style_cache *C, void *p, size_t sz) {
	struct attrib_cb *A = &C->free_attrib;
	if (sz <= ATTRIB_CB_SIZE && A->small_n < ATTRIB_CB_NODE) {
		memcpy(A->data[A->small_n], p, sz);
		++A->small_n;
	} else {
		if (A->n <= A->cap) {
			int newcap = (A->n + 1) * 2;
			A->big = (struct blob *)style_realloc(C, A->big, newcap * sizeof(struct blob), A->cap * sizeof(struct blob));
			A->cap = newcap;
		}
		struct blob * b = &A->big[A->n++];
		b->ptr = (char *)style_malloc(C, sz);
		b->sz = sz;
		memcpy(b->ptr, p, sz);
	}
}

static void
attrib_cb_iter(struct attrib_cb *A, struct style_cache *C, style_free_iter cb, void *ud) {
	int i;
	if (cb == NULL) {
		for (i=0;i<A->n;i++) {
			style_free(C, A->big[i].ptr, A->big[i].sz);
		}
	} else {
		for (i=0;i<A->small_n;i++) {
			cb(A->data[i], ud);
		}
		for (i=0;i<A->n;i++) {
			cb(A->big[i].ptr, ud);
			style_free(C, A->big[i].ptr, A->big[i].sz);
		}
	}
	A->small_n = 0;
	A->n = 0;
}

struct style_cache *
style_newcache(const unsigned char inherit_mask[128], style_alloc alloc, void *alloc_ud) {
	if (alloc == NULL) {
		alloc = default_alloc;
	}
	struct style_cache * c = (struct style_cache *)alloc(alloc_ud, NULL, 0, sizeof(*c));
	c->alloc = alloc;
	c->alloc_ud = alloc_ud;
	c->A = attrib_newstate(inherit_mask, c, attrib_cb_add);
	c->s = (struct style *)style_malloc(c, ARENA_DEFAULT_SIZE * sizeof(struct style));
	c->D = dirtylist_create(c);
	c->n = 0;
	c->cap = ARENA_DEFAULT_SIZE;
	c->freelist = -1;
	c->live = -1;
	c->dead = -1;
	c->empty = style_create(c, 0, NULL);
	attrib_cb_init(&c->free_attrib);
	return c;
}

void
style_deletecache(struct style_cache *c, style_free_iter cb, void *ud) {
	if (c == NULL)
		return;
	style_free(c, c->s, c->cap * sizeof(struct style));
	attrib_close(c->A, c);
	dirtylist_release(c->D);
	style_free(c, c, sizeof(*c));
	attrib_cb_iter(&c->free_attrib, c, cb, ud);
	attrib_cb_deinit(&c->free_attrib, c);
}

style_handle_t
style_null(struct style_cache *C) {
	return C->empty;
}

static int
alloc_style(struct style_cache *c) {
	if (c->freelist >= 0) {
		int r = c->freelist;
		struct style *s = &c->s[r];
		c->freelist = s->next;
		return r;
	}
	if (c->n >= c->cap) {
		int newcap = c->cap * 3 / 2;
		c->s = (struct style *)style_realloc(c, c->s, c->cap * sizeof(struct style), newcap * sizeof(struct style));
		c->cap = newcap;
	}
	return c->n++;
}

static void
link_to(struct style_cache *C, int id, int *node) {
	struct style *s = &C->s[id];
	s->prev = -1;
	s->next = *node;
	if (*node >= 0) {
		struct style *last = &C->s[*node];
		last->prev = id;
	}
	*node = id;
}

static void
remove_from(struct style_cache *C, int id, int *node) {
	struct style *s = &C->s[id];
	if (s->next >= 0) {
		struct style *n = &C->s[s->next];
		n->prev = s->prev;
	}
	if (s->prev < 0) {
		assert(*node == id);
		*node = s->next;
	} else {
		struct style *p = &C->s[s->prev];
		p->next = s->next;
	}
}

style_handle_t
style_create(struct style_cache *C, int n, struct style_attrib a[]) {
	struct attrib_state *A = C->A;
	assert(n <= MAX_KEY);
	int tmp[MAX_KEY];
	int i;
	for (i=0;i<n;i++) {
		tmp[i] = attrib_entryid(A, a[i].key, a[i].data, a[i].sz, C);
	}
	attrib_t attr = attrib_create(A, n, tmp, C);
	int id = alloc_style(C);
	struct style *s = &C->s[id];
	s->a = -1;
	s->b = -1;
	s->value = attr;
	s->refcount = 1;

	link_to(C, id, &C->live);

	style_handle_t r = { id };
	return r;
}

static inline struct style *
get_style(struct style_cache *C, int index) {
	assert(index >= 0 && index < C->n);
	struct style * s = &C->s[index];
	assert(s->refcount >= 0);
	return s;
}

#define DIRTYLIST_MAX 4096

static void make_dirty_list(struct style_cache *C, int id);

static inline void
make_dirty_(struct style_cache *C, int n, const int *array) {
	int i;
	for (i=0;i<n;i++) {
		int index = array[i];
		struct style *s = get_style(C, index);
		if (s->value.idx >= 0) {
			attrib_release(C->A, s->value, C);
			s->value.idx = -1;
			make_dirty_list(C, index);
		}
	}
}

static void
make_dirty_list(struct style_cache *C, int id) {
	int tmp[DIRTYLIST_MAX];
	int n = dirtylist_get(C->D, id, DIRTYLIST_MAX, tmp);
	if (n <= DIRTYLIST_MAX) {
		make_dirty_(C, n, tmp);
	} else {
		int * r = (int *)malloc(n * sizeof(int));
		dirtylist_get(C->D, id, n, r);
		make_dirty_(C, n, r);
		free(r);
	}
}

static inline void
make_dirty(struct style_cache *C, struct style *s, int id) {
	(void)s;	// unused (reserved for debug)
	make_dirty_list(C, id);
	assert(s->value.idx >= 0);
}

static inline int
is_value(struct style_cache *C, struct style *s) {
	return s->a < 0 && s->b < 0 && s->value.idx >= 0;
}

static inline int
is_combination(struct style_cache *C, struct style *s) {
	return s->a >= 0 && s->b >= 0;
}

int
style_modify(struct style_cache *C, style_handle_t h, int patch_n, struct style_attrib patch[]) {
	struct attrib_state *A = C->A;
	struct style *s = get_style(C, h.idx);
	assert(is_value(C, s));
	int tmp[MAX_KEY];
	int n = attrib_get(A, s->value, tmp);
	int i;
	int removed = 0;
	int change = 0;
	for (i=0;i<patch_n;i++) {
		int index = attrib_find(A, s->value, patch[i].key);
		if (index < 0) {
			if (patch[i].data) {
				// new
				int kv = attrib_entryid(A, patch[i].key, patch[i].data, patch[i].sz, C);
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
				int kv = attrib_entryid(A, patch[i].key, patch[i].data, patch[i].sz, C);
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
	attrib_t new_attr = attrib_create(A, n2, tmp, C);
	attrib_release(A, s->value, C);
	s->value = new_attr;
	make_dirty(C, s, h.idx);
	return 1;
}

void
style_addref(struct style_cache *C, style_handle_t h) {
	struct style *s = get_style(C, h.idx);
	if (++s->refcount == 1) {
		remove_from(C, h.idx, &C->dead);
		link_to(C, h.idx, &C->live);
	}
}

void
style_release(struct style_cache *C, style_handle_t h) {
	struct style *s = get_style(C, h.idx);
	if (--s->refcount <= 0) {
		assert(s->refcount == 0);
		remove_from(C, h.idx, &C->live);
		link_to(C, h.idx, &C->dead);
	}
}

static void
eval_(struct style_cache *C, style_handle_t h) {
	struct style *s = get_style(C, h.idx);
	if (s->value.idx >= 0)
		return;
	assert(is_combination(C, s));
	style_handle_t ah = { s->a };
	eval_(C, ah);
	style_handle_t bh = { s->b };
	eval_(C, bh);

	struct style *a = get_style(C, ah.idx);
	struct style *b = get_style(C, bh.idx);

	s->value = attrib_inherit(C->A, a->value, b->value, s->withmask, C);
}

int
style_assign(struct style_cache *C, style_handle_t h, style_handle_t v) {
	struct style *s = get_style(C, h.idx);
	assert(is_value(C, s));
	eval_(C, v);
	struct style *vv = get_style(C, v.idx);
	attrib_t attr = attrib_addref(C->A, vv->value);
	if (attr.idx == s->value.idx)	// no change
		return 0;
	attrib_release(C->A, s->value, C);
	s->value = attr;
	make_dirty(C, s, h.idx);
	return 1;
}

static inline void
addref(struct style_cache *C, int index) {
	struct style *p = get_style(C, index);
	if (++p->refcount == 1) {
		remove_from(C, index, &C->dead);
		link_to(C, index, &C->live);
	}
}

static void
add_affect(struct style_cache *C, int a, int b) {
	if (a < 0)
		return;
	dirtylist_add(C->D, a, b);
}

style_handle_t
style_inherit(struct style_cache *C, style_handle_t child, style_handle_t parent, int with_mask) {
	int id = alloc_style(C);
	struct style *s = &C->s[id];
	s->a = child.idx;
	s->b = parent.idx;
	s->value.idx = -1;
	s->refcount = 0;
	s->withmask = with_mask;

	link_to(C, id, &C->dead);

	addref(C, child.idx);
	addref(C, parent.idx);

	add_affect(C, s->a, id);
	add_affect(C, s->b, id);

	style_handle_t r = { id };
	return r;
}

static attrib_t
get_value(struct style_cache *C, style_handle_t h) {
	struct style *s = get_style(C, h.idx);
	if (s->value.idx < 0)
		eval_(C, h);
	return s->value;
}

void*
style_find(struct style_cache *C, style_handle_t h, uint8_t key) {
	attrib_t a = get_value(C, h);
	int index = attrib_find(C->A, a, key);
	if (index < 0)
		return NULL;
	return attrib_index(C->A, a, index, &key);
}

static void
dump_key(int indent, struct style_cache *C, int style_id, uint8_t key, char fmt) {
	if (style_id < 0)
		return;
	printf("%*s[%d] ", indent, "", style_id);
	struct style *s = get_style(C, style_id);
	if (s->value.idx < 0) {
		printf("DIRTY\n");
	} else {
		attrib_t a = s->value;
		int index = attrib_find(C->A, a, key);
		if (index < 0) {
			printf("%d\n", a.idx);
		} else {
			void *p = attrib_index(C->A, a, index, &key);
			printf("%d : ", a.idx);
			switch (fmt) {
			case 's':
				printf("%s", (const char *)p);
				break;
			case 'd':
				printf("%d", *(int *)p);
				break;
			case 'x':
				printf("%p", *(void **)p);
				break;
			case 'f':
				printf("%f", *(float *)p);
				break;
			case 'F':
				printf("%lf", *(double *)p);
				break;
			case 'p':
			default:
				printf("%p", p);
				break;
			}
			printf("\n");
		}
	}
	dump_key(indent+2, C, s->a, key, fmt);
	dump_key(indent+2, C, s->b, key, fmt);
}

void
style_dump_key(struct style_cache *C, style_handle_t h, uint8_t key, char fmt) {
	dump_key(0, C, h.idx, key, fmt);
}

void*
style_index(struct style_cache *C, style_handle_t h, int i, uint8_t *key) {
	attrib_t a = get_value(C, h);
	return attrib_index(C->A, a, i, key);
}

void
style_flush(struct style_cache *C, style_free_iter cb, void *ud) {
	int dead = C->dead;
	if (dead < 0)
		return;

	for (;;) {
		struct style *s = &C->s[dead];
		if (s->refcount < 0)
			break;
		do {
			struct style *s = &C->s[dead];
			if (s->refcount < 0)
				break;
			assert(s->refcount == 0);
			s->refcount = -1;
			if (s->a >= 0) {
				style_handle_t t = { s->a };
				style_release(C, t);
			}
			if (s->b >= 0) {
				style_handle_t t = { s->b };
				style_release(C, t);
			}
			dead = s->next;
		} while (dead >= 0);
		dead = C->dead;
	}

	for (;;) {
		struct style *s = &C->s[dead];
		dirtylist_clear(C->D, dead);
		if (s->next < 0) {
			s->next = C->freelist;
			C->freelist = C->dead;
			C->dead = -1;
			break;
		}
		dead = s->next;
	}

	attrib_cb_iter(&C->free_attrib, C, cb, ud);
}

#ifdef STYLE_TEST_MAIN

struct test_alloc {
	size_t sz;
};

static void *
test_alloc_func(void *ud, void *ptr, size_t osize, size_t nsize) {
	struct test_alloc *a = (struct test_alloc *)ud;
	if (nsize == 0) {
		a->sz -= osize;
		free(ptr);
		return NULL;
	} else {
		a->sz += nsize;
		a->sz -= osize;
		return realloc(ptr, nsize);
	}
}

#define STR(s) s, sizeof(s"")

static void
print_handle(struct style_cache *C, style_handle_t handle) {
	printf("HANDLE = %d\n", handle.idx);

	int i;
	for (i=0;;i++) {
		uint8_t key;
		void* v = style_index(C, handle, i, &key);
		if (v) {
			printf("\tKey = %d , Value = %s\n", key, (const char *)v);
		}
		else {
			break;
		}
	}
}

static void
free_attrib(void *ptr, void *ud) {
	printf("FREE %s\n", (const char *)ptr);
}

int
main() {
	unsigned char inherit_mask[MAX_KEY] = { 0 };
	struct test_alloc info;
	struct style_cache * C = style_newcache(inherit_mask, test_alloc_func, &info);

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

	printf("h1 = %d, h2 = %d\n", h1.idx, h2.idx);

	style_handle_t h3 = style_inherit(C, h1, h2, 0);
	style_handle_t h4 = style_inherit(C, h1, h2, 0);	// will release after style_flush()
	style_inherit(C, h4, h3, 0);	// will release after style_flush()

	style_addref(C, h3);

	print_handle(C, h3);

	style_flush(C, free_attrib, NULL);

	// Modify

	struct style_attrib patch[] = {
		{ NULL, 1 },	// remove 1
		{ STR("WORLD"), 2 },
	};

	printf("H1 = %d, h3 = %d\n", h1.idx, h3.idx);

	style_modify(C, h1, sizeof(patch)/sizeof(patch[0]), patch);

	style_dump_key(C, h3, 1, 's');

	print_handle(C, h3);

	style_handle_t h5 = style_null(C);

	h5 = style_inherit(C, h5, h3, 0);

	style_dump_key(C, h5, 1, 'p');

	print_handle(C, h5);

	style_release(C, h3);

	style_flush(C, free_attrib, NULL);

	style_deletecache(C, free_attrib, NULL);

	assert(info.sz == 0);

	return 0;
}
#endif
