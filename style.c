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

struct style_cache {
	style_alloc alloc;
	void * alloc_ud;
	struct attrib_state *A;
	struct style *s;
	struct dirtylist *D;
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

struct style_cache *
style_newcache(const unsigned char inherit_mask[128], style_alloc alloc, void *alloc_ud) {
	if (alloc == NULL) {
		alloc = default_alloc;
	}
	struct style_cache * c = (struct style_cache *)alloc(alloc_ud, NULL, 0, sizeof(*c));
	c->alloc = alloc;
	c->alloc_ud = alloc_ud;
	c->A = attrib_newstate(inherit_mask, c);
	c->s = (struct style *)style_malloc(c, ARENA_DEFAULT_SIZE * sizeof(struct style));
	c->D = dirtylist_create(c);
	c->n = 0;
	c->cap = ARENA_DEFAULT_SIZE;
	c->freelist = -1;
	c->live = -1;
	c->dead = -1;
	c->empty = style_create(c, 0, NULL);
	return c;
}

void
style_deletecache(struct style_cache *c) {
	if (c == NULL)
		return;
	style_free(c, c->s, c->cap * sizeof(struct style));
	attrib_close(c->A, c);
	dirtylist_release(c->D);
	style_free(c, c, sizeof(*c));
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

int
style_attrib_id(struct style_cache *C, const struct style_attrib *attrib) {
	return attrib_entryid(C->A, attrib->key, attrib->data, attrib->sz, C);
}

void
style_attrib_value(struct style_cache *C, int id, struct style_attrib *attrib) {
	attrib->data = attrib_entry_get(C->A, id, &attrib->key, &attrib->sz);
}

void
style_attrib_addref(struct style_cache *C, int id) {
	attrib_entry_addref(C->A, id);
}

void
style_attrib_release(struct style_cache *C, int id) {
	attrib_entry_release(C->A, id, C);
}

style_handle_t
style_create(struct style_cache *C, int n, const int tmp[]) {
	struct attrib_state *A = C->A;
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
style_modify(struct style_cache *C, style_handle_t h, int patch_n, int patch[], int removed_n, int removed_key[]) {
	struct attrib_state *A = C->A;
	struct style *s = get_style(C, h.idx);
	assert(is_value(C, s));
	int tmp[MAX_KEY];
	int n = attrib_get(A, s->value, tmp);
	int i;
	int change = 0;
	for (i=0;i<patch_n;i++) {
		struct style_attrib attr;
		style_attrib_value(C, patch[i], &attr);
		int index = attrib_find(A, s->value, attr.key);
		if (index < 0) {
			// new
			tmp[n++] = patch[i];
			assert(n <= MAX_KEY);
			patch[i] = 1;
			change = 1;
		} else {
			if (tmp[index] != patch[i]) {
				change = 1;
				tmp[index] = patch[i];
				patch[i] = 1;
			} else {
				patch[i] = 0;
			}
		}
	}
	if (!change && removed_n == 0)
		return 0;
	for (i=0;i<removed_n;i++) {
		int index = attrib_find(A, s->value, removed_key[i]);
		if (index >= 0) {
			tmp[index] = -1;
			removed_key[i] = 1;
		} else {
			removed_key[i] = 0;
		}
	}
	int i2 = 0;
	int n2 = n;
	for (i=0;i<n && removed_n > 0;i++) {
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
style_compare(struct style_cache *C, style_handle_t h, style_handle_t v) {
	struct style *s = get_style(C, h.idx);
	assert(is_value(C, s));
	eval_(C, v);
	struct style *vv = get_style(C, v.idx);
	return vv->value.idx != s->value.idx;
}

int
style_assign(struct style_cache *C, style_handle_t h, style_handle_t v) {
	if (style_compare(C, h, v)) {
		struct style *vv = get_style(C, v.idx);
		attrib_t attr = attrib_addref(C->A, vv->value);
		struct style *s = get_style(C, h.idx);
		attrib_release(C->A, s->value, C);
		s->value = attr;
		make_dirty(C, s, h.idx);
		return 1;
	}
	return 0;
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

int
style_find(struct style_cache *C, style_handle_t h, uint8_t key) {
	attrib_t a = get_value(C, h);
	int index = attrib_find(C->A, a, key);
	if (index < 0)
		return -1;
	return attrib_index(C->A, a, index);
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
			int id = attrib_index(C->A, a, index);
			struct style_attrib attr;
			style_attrib_value(C, id, &attr);
			void *p = attr.data;
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

int
style_index(struct style_cache *C, style_handle_t h, int i) {
	attrib_t a = get_value(C, h);
	return attrib_index(C->A, a, i);
}

void
style_flush(struct style_cache *C) {
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
			return;
		}
		dead = s->next;
	}
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
		int id = style_index(C, handle, i);
		if (id >= 0) {
			struct style_attrib attr;
			style_attrib_value(C, id, &attr);
			printf("\tKey = %d , Value = %s\n", attr.key, (const char *)attr.data);
		}
		else {
			break;
		}
	}
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

	int id_a[] = {
		style_attrib_id(C, &a[0]),
		style_attrib_id(C, &a[1]),
	};

	style_handle_t h1 = style_create(C, sizeof(id_a)/sizeof(id_a[0]), id_a);

	struct style_attrib b[] = {
		{ STR("hello world"), 1 },
		{ STR("world"), 2 },
	};

	int id_b[] = {
		style_attrib_id(C, &b[0]),
		style_attrib_id(C, &b[1]),
	};

	style_handle_t h2 = style_create(C, sizeof(b)/sizeof(b[0]), id_b);

	printf("h1 = %d, h2 = %d\n", h1.idx, h2.idx);

	style_handle_t h3 = style_inherit(C, h1, h2, 0);
	style_handle_t h4 = style_inherit(C, h1, h2, 0);	// will release after style_flush()
	style_inherit(C, h4, h3, 0);	// will release after style_flush()

	style_addref(C, h3);

	print_handle(C, h3);

	style_flush(C);

	// Modify

	struct style_attrib kv = {
		STR("WORLD"), 2
	};

	int patch[] = { style_attrib_id(C, &kv) };
	int removed[] = { 1 };

	printf("H1 = %d, h3 = %d\n", h1.idx, h3.idx);

	style_modify(C, h1, 1, patch, 1, removed);

	style_dump_key(C, h3, 1, 's');

	print_handle(C, h3);

	style_handle_t h5 = style_null(C);

	h5 = style_inherit(C, h5, h3, 0);

	style_dump_key(C, h5, 1, 'p');

	print_handle(C, h5);

	style_release(C, h3);

	style_flush(C);

	style_deletecache(C);

	assert(info.sz == 0);

	return 0;
}
#endif
