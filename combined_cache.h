#ifndef combined_cache_h
#define combined_cache_h

#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "attrib.h"
#include "hash.h"

#define COMBINE_CACHE_BITS 12
#define COMBINE_CACHE_SIZE_MASK ((1 << COMBINE_CACHE_BITS) - 1)
#define COMBINE_CACHE_SIZE COMBINE_CACHE_SIZE_MASK // 4095
#define COMBINE_CACHE_HASH_SIZE (1<<13) // 8192, 2x size of COMBINE_CACHE_SIZE
#define COMBINE_CACHE_HASH_SHIFT (32-13)
#define COMBINE_CACHE_HASH_C (sizeof(uint64_t) * 8 / COMBINE_CACHE_BITS)	// 5

struct combined_node {
	uint64_t id;
	uint64_t a;
	uint64_t b;
	attrib_t data;
	uint32_t value:1;
	uint32_t mask:1;
	uint32_t list:30;
};

struct combined_cache {
	int head;
	int tail;
	struct combined_node queue[COMBINE_CACHE_SIZE];	// A LRU queue
	uint64_t id_index[COMBINE_CACHE_HASH_SIZE];
	uint64_t combined_index[COMBINE_CACHE_HASH_SIZE];
};

static inline struct combined_node *
get_node(struct combined_cache *c, int index) {
	assert(index >= 0);
	return &c->queue[index];
}

static inline uint32_t
make_list_(int prev, int next) {
	uint16_t n[2] = {
		(prev + 1) & COMBINE_CACHE_SIZE_MASK,
		(next + 1) & COMBINE_CACHE_SIZE_MASK,
	};
	return n[0] << COMBINE_CACHE_BITS | n[1];
}

static inline int
prev_list_(uint32_t list) {
	return ((list >> COMBINE_CACHE_BITS) & COMBINE_CACHE_SIZE_MASK) - 1;
}

static inline int
next_list_(uint32_t list) {
	return (list & COMBINE_CACHE_SIZE_MASK) - 1;
}

static inline void
set_prev_(struct combined_node *node, int prev) {
	node->list = make_list_(prev, next_list_(node->list));
}

static inline void
set_next_(struct combined_node *node, int next) {
	node->list = make_list_(prev_list_(node->list), next);
}

// for debug
static void
combined_cache_dump(struct combined_cache *c) {
	int index = c->head;
	printf("Combined cache:\n");
	while (index > 0) {
		struct combined_node *node = &c->queue[index];
		if (node->id != 0) {
			printf("\t[%d] : id = %" PRIx64 "\ta = %" PRIx64 "\tb = %" PRIx64 " %s", index, node->id, node->a, node->b, node->mask ? "*":"");
			if (node->value) {
				printf("\tattrib = %x", node->data.idx);
			}
			printf("\n");
		}
		index = next_list_(node->list);
	}
}


static void
combined_cache_init(struct combined_cache *c) {
	memset(c, 0, sizeof(*c));
	int i;
	for (i=0;i<COMBINE_CACHE_SIZE-1;i++) {
		c->queue[i].list = make_list_(i-1, i+1);
	}
	c->queue[i].list = make_list_(i-1, -1);
	c->head = 0;
	c->tail = COMBINE_CACHE_SIZE-1;
}

static inline int
cache_hash_id_(uint64_t id) {
	return int32_hash(id>>1) >> COMBINE_CACHE_HASH_SHIFT;
}

static inline int
cache_hash_combined_(uint64_t a, uint64_t b) {
	return int32_hash((a & 0xffff) | (( b & 0xffff) << 16)) >> COMBINE_CACHE_HASH_SHIFT;
}

static struct combined_node *
lookup_combined_(struct combined_cache *c, int hash, uint64_t a, uint64_t b) {
	int i;
	uint64_t v = c->combined_index[hash];
	for (i=0;i<COMBINE_CACHE_HASH_C;i++) {
		uint32_t index = v & COMBINE_CACHE_SIZE_MASK;
		if (index == 0) {
			return NULL;
		}
		struct combined_node *n = &c->queue[index-1];
		if (n->a == a && n->b == b)
			return n;
		v >>= COMBINE_CACHE_BITS;
	}

	for (i=0;i<COMBINE_CACHE_SIZE;i++) {
		struct combined_node *n = &c->queue[i];
		if (n->a == a && n->b == b)
			return n;
	}

	return NULL;
}

static struct combined_node *
combined_cache_find_notouch(struct combined_cache *c, uint64_t id) {
	int hash = cache_hash_id_(id);
	int i;
	struct combined_node *n;
	uint64_t v = c->id_index[hash];
	for (i=0;i<COMBINE_CACHE_HASH_C;i++) {
		int index = v & COMBINE_CACHE_SIZE_MASK;
		if (index == 0)
			return NULL;
		n = &c->queue[index-1];
		if (n->id == id)
			return n;
		v >>= COMBINE_CACHE_BITS;
	}

	for (i=0;i<COMBINE_CACHE_SIZE;i++) {
		struct combined_node *n = &c->queue[i];
		if (n->id == id)
			return n;
	}

	return NULL;
}

static inline void
cache_touch_(struct combined_cache *c, struct combined_node *node) {
	int slot = node - c->queue;
	if (slot == c->head)
		return;

	// remove it from list

	if (slot == c->tail) {
		int tail = prev_list_(node->list);
		c->tail = tail;
		struct combined_node * tail_node = get_node(c, tail);
		set_next_(tail_node, -1);
	} else {
		int prev = prev_list_(node->list);
		int next = next_list_(node->list);
		struct combined_node * prev_node = get_node(c, prev);
		struct combined_node * next_node = get_node(c, next);
		set_next_(prev_node, next);
		set_prev_(next_node, prev);
	}

	// move it to head

	int head = c->head;
	struct combined_node * head_node = &c->queue[head];
	set_prev_(head_node, slot);
	c->head = slot;
	node->list = make_list_(-1, head);
}

static struct combined_node *
combined_cache_find(struct combined_cache *c, uint64_t id) {
	struct combined_node * n = combined_cache_find_notouch(c, id);
	if (n) {
		cache_touch_(c, n);
	}
	return n;
}

static int
remove_index_(uint64_t queue[COMBINE_CACHE_HASH_SIZE], int index, int slot) {
	uint64_t v = queue[index];
	uint64_t nv = 0;
	int i;
	int dirty = 0;
	for (i=0;i<COMBINE_CACHE_HASH_C;i++) {
		int index = v & COMBINE_CACHE_SIZE_MASK;
		if (index == 0)
			break;
		if (index != slot + 1) {
			nv = nv << COMBINE_CACHE_BITS | index;
		} else {
			dirty = 1;
		}
		v >>= COMBINE_CACHE_BITS;
	}
	if (dirty) {
		queue[index] = nv;
	}
	return (i == COMBINE_CACHE_HASH_C && dirty);
}

static inline void
add_node_index_(uint64_t queue[COMBINE_CACHE_HASH_SIZE], int index, int slot) {
	queue[index] = queue[index] << COMBINE_CACHE_BITS | (slot + 1);
}

static void
remove_node_index_(struct combined_cache *c, int slot) {
	struct combined_node *node = get_node(c, slot);
	int h = cache_hash_combined_(node->a, node->b);
	int i;
	if (remove_index_(c->combined_index, h, slot)) {
		c->combined_index[h] = 0;
		for (i=0;i<COMBINE_CACHE_SIZE;i++) {
			if (i != slot) {
				struct combined_node *n = &c->queue[i];
				if (n->id && h == cache_hash_combined_(n->a, n->b)) {
					add_node_index_(c->combined_index, h, i);
				}
			}
		}
	}
	h = cache_hash_id_(node->id);
	if (remove_index_(c->id_index, h, slot)) {
		c->id_index[h] = 0;
		for (i=0;i<COMBINE_CACHE_SIZE;i++) {
			if (i != slot) {
				struct combined_node *n = &c->queue[i];
				if (n->id && h == cache_hash_id_(n->id)) {
					add_node_index_(c->id_index, h, i);
				}
			}
		}
	}
}


static void
make_node_index_(struct combined_cache *c, int slot) {
	struct combined_node *node = get_node(c, slot);
	int h = cache_hash_combined_(node->a, node->b);
	add_node_index_(c->combined_index, h, slot);
	h = cache_hash_id_(node->id);
	add_node_index_(c->id_index, h, slot);
}

// New a combined node with (a,b) or use an exist one
static uint64_t
combined_cache_new(struct combined_cache *c, uint64_t a, uint64_t b, int mask, uint64_t *idbase, struct attrib_state *A) {
	int h = cache_hash_combined_(a, b);
	struct combined_node *node = lookup_combined_(c, h, a, b);
	if (node) {
		assert(node->mask == mask);
		return node->id;
	}
	*idbase += 2;

	uint64_t id = *idbase + 1;

	// remove tail

	int slot = c->tail;
	node = &c->queue[slot];
	remove_node_index_(c, slot);
	if (node->value) {
		attrib_release(A, node->data);
	}

	int prev = prev_list_(node->list);
	struct combined_node *tail_node = get_node(c, prev);
	set_next_(tail_node, -1);
	c->tail = prev;

	// reuse old tail slot
	
	node->id = id;
	node->a = a;
	node->b = b;
	node->value = 0;
	node->mask = mask;

	node->list = make_list_(-1, c->head);

	// put it (slot) to head

	struct combined_node *head_node = get_node(c, c->head);
	set_prev_(head_node, slot);
	c->head = slot;

	make_node_index_(c, slot);

	return id;
}

static void
combined_cache_check(struct combined_cache *C) {
	int i;
	int index = C->head;
	for (i=0;i<COMBINE_CACHE_SIZE-1;i++) {
		struct combined_node *node = &C->queue[index];
		int h = cache_hash_combined_(node->a, node->b);
		struct combined_node *c = lookup_combined_(C, h, node->a, node->b);
		struct combined_node *v = combined_cache_find_notouch(C, node->id);
		assert(c == v);
		assert(node == c || c == NULL);

		index = next_list_(node->list);
	}
	assert(index == C->tail);
}

#endif
