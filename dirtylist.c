#include "dirtylist.h"

#include <stdlib.h>
#include <assert.h>

#define DIRTYLIST_INITSIZE 1024

struct dirtypair {
	int a;
	int b;
	int next;
};

struct dirtylist {
	int cap;
	int n;
	int freelist;
	struct dirtypair p[1];
};

struct dirtylist *
dirtylist_expand(struct dirtylist *D, struct style_cache *C) {
	if (D == NULL) {
		int cap = DIRTYLIST_INITSIZE;
		size_t sz = sizeof(*D) + (cap - 1) * sizeof(struct dirtypair);
		D = (struct dirtylist *)style_malloc(C, sz);
		D->cap = cap;
		D->n = 0;
		D->freelist = -1;
		return D;
	}
	int cap = D->cap * 3 / 2;
	size_t osz = sizeof(*D) + (D->cap - 1) * sizeof(struct dirtypair); 
	size_t sz = sizeof(*D) + (cap - 1) * sizeof(struct dirtypair);
	struct dirtylist * ret = (struct dirtylist *)style_realloc(C, D, osz, sz);
	if (ret == NULL)
		return D;
	ret->cap = cap;
	return ret;
}

void
dirtylist_release(struct dirtylist *D, struct style_cache *C) {
	if (D == NULL)
		return;
	size_t sz = sizeof(*D) + (D->cap - 1) * sizeof(struct dirtypair);
	style_free(C, D, sz);
}

int
dirtylist_add(struct dirtylist *D, int a, int b, int next) {
	if (D == NULL) {
		return -1;
	}
	int index = D->freelist;
	struct dirtypair * p;
	if (index >= 0) {
		p = &D->p[index];
		D->freelist = p->next;
	} else if (D->n < D->cap) {
		index = D->n++;
		p = &D->p[index];
	} else {
		return -1;
	}
	assert(a >= 0 && b >= 0);
	p->a = a;
	p->b = b;
	p->next = next;
	return index;
}

void
dirtylist_clear(struct dirtylist *D, int a) {
	if (D == NULL)
		return;
	int i;
	int freelist = -1;
	for (i=0;i<D->n;i++) {
		struct dirtypair * p = &D->p[i];
		if (p->a >= 0) {
			if (p->a == a) {
				p->a = -1;
				p->next = freelist;
				freelist = i;
			} else {
				if (p->b == a) {
					// mark only, returns to freelist during dirtylist_next
					p->b = -1;
				}
			}
		} else {
			p->next = freelist;
			freelist = i;
		}
	}
	D->freelist = freelist;
}

int*
dirtylist_next(struct dirtylist *D, int *index, int *value) {
	int current = *index;
	if (current < 0)
		return NULL;
	struct dirtypair * p = &D->p[current];
	while (p->b < 0) {
		int next = p->next;
		p->a = -1;
		p->next = D->freelist;
		D->freelist = current;
		if (next < 0) {
			*index = -1;
			return NULL;
		}
		p = &D->p[next];
		current = next;
	}
	*index = current;
	*value = p->b;
	return &(p->next);
}

void
dirtylist_check(struct dirtylist *D, int index, int v) {
	struct dirtypair * p = &D->p[index];
	assert(p->a == v);
}

#include <stdio.h>

void
dirtylist_dump(struct dirtylist *D) {
	int i;
	for (i=0;i<D->n;i++) {
		struct dirtypair * p = &D->p[i];
		if (p->a >= 0) {
			printf("[%d] : %d -> %d (%d)\n",
				i, p->a, p->b, p->next);
		}
	}
}

#ifdef DIRTYLIST_TEST_MAIN

#include "style.h"

static void
print_list(struct dirtylist *D, int *head, int v) {
	int value;
	printf("%d : ", v);
	while ((head = dirtylist_next(D, head, &value))) {
		printf("%d ", value);
	}
	printf("\n");
}

int
main() {
	struct style_cache *C = style_newcache(NULL, NULL, NULL);
	struct dirtylist * D = dirtylist_expand(NULL, C);

	int head[4] = { -1, -1 , -1, -1 };

	head[0] = dirtylist_add(D, 0, 1, head[0]);	// add 0->1
	head[0] = dirtylist_add(D, 0, 2, head[0]);	// add 0->2
	head[0] = dirtylist_add(D, 0, 3, head[0]);	// add 0->3

	head[1] = dirtylist_add(D, 1, 2, head[1]);	// add 1->2

	head[2] = dirtylist_add(D, 2, 3, head[2]);	// add 2->3
	head[2] = dirtylist_add(D, 2, 0, head[1]);	// add 2->0

	print_list(D, &head[0], 0);
	print_list(D, &head[1], 1);
	print_list(D, &head[2], 2);
	dirtylist_dump(D);

	dirtylist_clear(D, 2);
	head[2] = -1;

	print_list(D, &head[1], 1);
	print_list(D, &head[0], 0);
	dirtylist_dump(D);

	dirtylist_release(D, C);

	return 0;
}

#endif





