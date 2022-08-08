#ifndef intern_cache_h
#define intern_cache_h

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

//#define TEST_INTERN

#ifdef TEST_INTERN

#define VERIFY struct verify_cache v;

#define MAX_INDEX 0x10000

struct verify_cache {
	int n;
	uint32_t index[MAX_INDEX];
};

#else

#define VERIFY

#endif

#define INVALID_INDEX (~0)

struct intern_cache {
	int size;
	int shift;
	int collide_n;
	int n;
	uint32_t *index;	// 2x n hash map
	uint32_t *collide;	// n collide array
	VERIFY
};


#ifdef TEST_INTERN

static inline void
verify_init(struct intern_cache *C) {
	struct verify_cache *v = &C->v;
	v->n = 0;
}

static inline void
verify_insert(struct intern_cache *C, uint32_t index) {
	struct verify_cache *v = &C->v;
	assert(v->n <= MAX_INDEX);
	int i;
	for (i=0;i<v->n;i++) {
		assert(v->index[i] != index);
	}
	v->index[v->n++] = index;
}

static inline void
verify_remove(struct intern_cache *C, uint32_t index) {
	int i;
	struct verify_cache *v = &C->v;
	for (i=0;i<v->n;i++) {
		if (v->index[i] == index) {
			--v->n;
			v->index[v->i] = v->index[n];
			return;
		}
	}
	assert(0);
}

#else

static inline void verify_init(struct intern_cache *C) {};
static inline void verify_insert(struct intern_cache *C, uint32_t index) {}
static inline void verify_remove(struct intern_cache *C, uint32_t index) {};

#endif

struct intern_cache_iterator {
	uint32_t result;
	int collide;
};

static inline void
intern_cache_init(struct intern_cache *c, int bits) {
	memset(c, 0, sizeof(*c));
	verify_init(c);
	c->n = 0;
	c->size =  1 << bits;
	c->shift = 32 - bits - 1;
	c->collide = (uint32_t *)malloc(c->size * 3 * sizeof(uint32_t));
	c->index = c->collide + c->size;
	memset(c->index, 0xff, c->size * 2 * sizeof(uint32_t));
}

static inline void
intern_cache_deinit(struct intern_cache *c) {
	free(c->collide);
}

static inline size_t
intern_cache_memsize(struct intern_cache *c) {
	return c->size * 3 * sizeof(uint32_t);
}

typedef uint32_t (*hash_get_func)(uint32_t index, void *ud);

static inline void
insert_(struct intern_cache *c, uint32_t index, int p) {
	assert(c->collide_n < c->size);
	memmove(c->collide + p + 1, c->collide + p, (c->collide_n - p) * sizeof(uint32_t));
	c->collide[p] = index;
	++c->collide_n;
}

static inline void
intern_cache_insert_(struct intern_cache *c, uint32_t index, hash_get_func hash, void *ud) {
	uint32_t h = hash(index, ud);
	int mainslot = h >> c->shift;
	if (c->index[mainslot] != INVALID_INDEX) {
		uint32_t cindex = c->index[mainslot];
		assert(cindex != index);
		int begin = 0;
		int end = c->collide_n;
		while (begin < end) {
			int mid = (begin + end) / 2;
			uint32_t i = c->collide[mid];
			uint32_t mid_h = hash(i, ud);
			if (h == mid_h) {
				assert(i != cindex);
				insert_(c, cindex, mid);
				c->index[mainslot] = index;
				return;
			} else if (h < mid_h) {
				end = mid;
			} else {
				begin = mid + 1;
			}
		}
		insert_(c, cindex, begin);
	}
	c->index[mainslot] = index;
}

static inline void
intern_cache_resize_(struct intern_cache *c, int bits, hash_get_func hash, void *ud) {
	uint32_t *index = c->index;
	uint32_t *collide = c->collide;
	int size = c->size;
	int collide_n = c->collide_n;
	int n = c->n;
	intern_cache_init(c, bits);
	c->n = n;
	int i;
	for (i=0;i<collide_n;i++) {
		intern_cache_insert_(c, collide[i], hash, ud);
	}
	for (i=0;i<size*2;i++) {
		if (index[i] != INVALID_INDEX) {
			intern_cache_insert_(c, index[i], hash, ud);
		}
	}
	free(collide);
}

static inline void
intern_cache_insert(struct intern_cache *c, uint32_t index, hash_get_func hash, void *ud) {
	verify_insert(c, index);
	++c->n;
	if (c->n >= c->size) {
		int bits = 31 - c->shift;
		intern_cache_resize_(c, bits+1, hash, ud);
	}
	intern_cache_insert_(c, index, hash, ud);
}

static inline int
lower_bound_(struct intern_cache *c, uint32_t h, int begin, int end, hash_get_func hash, void *ud) {
	while (begin < end) {
		int mid = (begin + end) / 2;
		uint32_t i = c->collide[mid];
		uint32_t mid_h = hash(i, ud);
		if (h == mid_h) {
			int bound = lower_bound_(c, h, begin, mid , hash, ud);
			if (bound >= 0)
				return bound;
			else
				return mid;
		} else if (h < mid_h) {
			end = mid;
		} else {
			begin = mid + 1;
		}
	}
	return -1;
}

// return 0 : not found
static inline int
intern_cache_find(struct intern_cache *c, uint32_t h, struct intern_cache_iterator *iter, hash_get_func hash, void *ud) {
	int mainslot = h >> c->shift;
	uint32_t v = c->index[mainslot];
	if (v == INVALID_INDEX)
		return 0;
	if (hash(v, ud) == h) {
		iter->result = v;
		iter->collide = -1;
		return 1;
	}
	iter->collide = lower_bound_(c, h, 0, c->collide_n, hash, ud);
	if (iter->collide < 0)
		return 0;
	iter->result = c->collide[iter->collide];
	return 1;
}

static inline int
intern_cache_find_next(struct intern_cache *c, struct intern_cache_iterator *iter, hash_get_func hash, void *ud) {
	if (iter->collide < 0) {
		uint32_t h = hash(iter->result, ud);
		iter->collide = lower_bound_(c, h, 0, c->collide_n, hash, ud);
		if (iter->collide < 0)
			return 0;
		iter->result = c->collide[iter->collide];
		return 1;
	} else {
		++iter->collide;
		if (iter->collide >= c->collide_n)
			return 0;
		uint32_t h = hash(iter->result, ud);
		iter->result = c->collide[iter->collide];
		uint32_t nexth = hash(iter->result, ud);
		return (h == nexth);
	}
}

static inline void
remove_collide_(struct intern_cache *c, int cindex) {
	assert(cindex < c->collide_n);
	--c->collide_n;
	memmove(c->collide + cindex, c->collide + cindex + 1, (c->collide_n - cindex) * sizeof(uint32_t));
}

static inline void
intern_cache_remove(struct intern_cache *c, uint32_t index, hash_get_func hash, void *ud) {
	verify_remove(c, index);
	struct intern_cache_iterator iter;
	uint32_t h = hash(index, ud);
	int found = 0;
	if (intern_cache_find(c, h, &iter, hash, ud)) {
		do {
			if (iter.result == index) {
				found = 1;
				break;
			}
		} while (intern_cache_find_next(c, &iter, hash, ud));
	}
	assert(found);
	--c->n;
	if (iter.collide < 0) {
		uint32_t h = hash(iter.result, ud);
		int i = h >> c->shift;
		assert(c->index[i] == iter.result);
		int cindex = lower_bound_(c, h, 0, c->collide_n, hash, ud);
		if (cindex < 0) {
			c->index[i] = INVALID_INDEX;
		} else {
			c->index[i] = c->collide[cindex];
			remove_collide_(c, cindex);
		}
	} else {
		remove_collide_(c, iter.collide);
	}
}

#endif
