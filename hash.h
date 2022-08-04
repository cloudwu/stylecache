#ifndef STYLE_HASH_H
#define STYLE_HASH_H

#include <stdint.h>

// 2654435769 = ((sqrt(5)-1)/2) * 2^32
#define KNUTH_HASH 2654435769

#define hash_mainslot(hash, h) ((hash) >> h->shift)

static inline uint32_t
array_hash(int *v, int n) {
	uint32_t *str = (uint32_t*)v;
	uint32_t h = (uint32_t)(n);
	int l = n;
	for (; l > 0; l--)
		h ^= ((h<<29) + (h>>2) + (str[l - 1]));
	h *= KNUTH_HASH;
	// hash 0 is reserved for empty slot
	if (h == 0)
		return 1;
	return h;
}

static inline uint32_t
kv_hash(int key, void *value, size_t l) {
	uint8_t *str = (uint8_t *)value;
	uint32_t h = (uint32_t)(key ^ l);
	for (; l > 0; l--)
		h ^= ((h<<5) + (h>>2) + (str[l - 1]));
	h *= KNUTH_HASH;
	return h;
}

static inline uint32_t
id64_hash(uint64_t id) {
	uint32_t v = (uint32_t)id;
    v *= KNUTH_HASH;
	return v;
}

static inline uint32_t
int32_hash(uint32_t v) {
    v *= KNUTH_HASH;
	return v;
}


#endif
