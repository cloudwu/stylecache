#include "intern_cache.h"
#include "hash.h"
#include "style.h"
#include <stdint.h>
#include <assert.h>
#include <stdio.h>

struct node {
	uint32_t hash;
	int value;
	int index;
};


static uint32_t
get_hash(uint32_t index, void *ud) {
	struct node * n = (struct node *)ud;
	return n[index].hash;
}

static void
print_value(struct intern_cache *cache, struct node *array, int value) {
	struct intern_cache_iterator iter;
	uint32_t hash = int32_hash(value/2);
	if (intern_cache_find(cache, hash, &iter, get_hash, array)) {
		do {
			struct node * n = &array[iter.result];
			if (n->value == value) {
				printf("result = %d Value = %d index = %d\n", (int)iter.result , n->value, n->index);
			}
		} while (intern_cache_find_next(cache, &iter, get_hash, array));
	}
}

int
main() {
	struct style_cache *C = style_newcache(NULL, NULL, NULL);
	struct intern_cache cache;
	struct node array[100];
	int i;

	intern_cache_init(C, &cache, 4);	// 16 slots
	for (i=0;i<10;i++) {
		array[i].hash = int32_hash(i/4);
		array[i].value = i/2;
		array[i].index = i;
		printf("INSERT [%d] %d\n", i, i/2);
		intern_cache_insert(&cache, i, get_hash, array, C);
	}

	for (i=0;i<5;i++) {
		print_value(&cache, array, i);
	}

	for (i=10;i<30;i++) {
		array[i].hash = int32_hash(i/4);
		array[i].value = i/2;
		array[i].index = i;
		printf("INSERT [%d] %d\n", i, i/2);
		intern_cache_insert(&cache, i, get_hash, array, C);
	}

	for (i=0;i<15;i++) {
		print_value(&cache, array, i);
	}

	intern_cache_deinit(C, &cache);

	style_deletecache(C);
	return 0;
}