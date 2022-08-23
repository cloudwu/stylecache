#ifndef inherit_cache_h
#define inherit_cache_h

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "hash.h" 

#define INHERIT_CACHE_INVALID_KEY 0xffffffff
#define INHERIT_CACHE_SIZE (1<<13)// 8192, 13bits
#define INHERIT_CACHE_SHIFT (32-13)

struct inherit_entry {
	uint32_t a;
	uint32_t b;
	uint32_t result;
	uint32_t withmask : 1;
	uint32_t a_version : 10;
	uint32_t b_version : 10;
	uint32_t r_version : 10;
};

struct inherit_cache {
	struct inherit_entry s[INHERIT_CACHE_SIZE];
	uint16_t *version;
	int n;
};

static inline void
inherit_cache_init(struct inherit_cache *c) {
	int i;
	for (i=0;i<INHERIT_CACHE_SIZE;i++) {
		c->s[i].a = INHERIT_CACHE_INVALID_KEY;
	}
	c->version = NULL;
	c->n = 0;
}

static inline void
inherit_cache_deinit(struct inherit_cache *c) {
	free(c->version);
}

static inline int
hash_inherit_combined_slot(int a, int b) {
	uint32_t v = (a & 0xffff) | ((b & 0xffff) << 16);
	v = int32_hash(v) >> INHERIT_CACHE_SHIFT;
	return (int)v;
}

static inline int
inherit_cache_fetch(struct inherit_cache *c, int a, int b, int withmask) {
	if (a >= c->n || b >= c->n)
		return -1;
	int v = hash_inherit_combined_slot(a,b);
	struct inherit_entry *e = &c->s[v];

	if (e->a == a && e->b == b 
		&& e->a_version == c->version[a]
		&& e->b_version == c->version[b]
		&& e->r_version == c->version[e->result]
		&& e->withmask == withmask)
		return e->result;
	return -1;
}

static inline void
clear_entry_with_key(struct inherit_cache *c, int key) {
	int i;
	for (i=0;i<INHERIT_CACHE_SIZE;i++) {
		struct inherit_entry *e = &c->s[i];
		if (e->a == key || e->b == key || e->result == key)
			e->a = INHERIT_CACHE_INVALID_KEY;
	}
}

static inline void
inherit_cache_retirekey(struct inherit_cache *c, int key) {
	if (key >= c->n)
		return;
	if (++c->version[key] == 0) {
		clear_entry_with_key(c, key);
	}
}

static inline void
resize_inherit_cache(struct inherit_cache *c, int a, int b, int result) {
	int size = INHERIT_CACHE_SIZE;
	if (c->n > size)
		size = c->n;
	while (size < a) size *= 2;
	while (size < b) size *= 2;
	while (size < result) size *= 2;
	if (size > c->n) {
		uint16_t * v = (uint16_t *)malloc(size * sizeof(uint16_t));
		memset(v, 0, sizeof(uint16_t) * size);
		memcpy(v, c->version, sizeof(uint16_t) * c->n);
		free(c->version);
		c->version = v;
		c->n = size;
	}
}

static inline void
inherit_cache_set(struct inherit_cache *c, int a, int b, int withmask, int result) {
	resize_inherit_cache(c, a, b, result);
	int v = hash_inherit_combined_slot(a,b);
	struct inherit_entry *e = &c->s[v];
	e->a = a;
	e->b = b;
	e->result = result;
	e->withmask = withmask;
	e->a_version = c->version[a];
	e->b_version = c->version[b];
	e->r_version = c->version[result];
}

#endif
