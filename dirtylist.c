#include "dirtylist.h"

#include <stdlib.h>
#include <assert.h>

#define DIRTYLIST_INITSIZE 1024

struct dirtyslot {
	unsigned int version;
	int b;
	int next;
};

struct dirtyhead {
	unsigned int version;
	int head;
};

struct dirtylist {
	struct style_cache *C;
	int cap;
	int n;
	int freelist;
	int maxid;
	struct dirtyhead *h;
	struct dirtyslot *p;
};

struct dirtylist *
dirtylist_create(struct style_cache *C) {
	struct dirtylist *D = (struct dirtylist *)style_malloc(C, sizeof(*D));
	D->C = C;
	D->cap = DIRTYLIST_INITSIZE;
	D->n = 0;
	D->freelist = -1;
	D->maxid = DIRTYLIST_INITSIZE;
	D->h = (struct dirtyhead *)style_malloc(C, D->maxid * sizeof(struct dirtyhead));
	int i;
	for (i=0;i<D->maxid;i++) {
		D->h[i].head = -1;
		D->h[i].version = 0;
	}
	D->p = (struct dirtyslot *)style_malloc(C, D->cap * sizeof(struct dirtyslot));
	return D;
}
void
dirtylist_release(struct dirtylist *D) {
	if (D == NULL)
		return;
	struct style_cache *C = D->C;
	style_free(C, D->h, D->maxid * sizeof(struct dirtyhead));
	style_free(C, D->p, D->cap * sizeof(struct dirtyslot));
	style_free(C, D, sizeof(*D));
}

void
dirtylist_add(struct dirtylist *D, int a, int b) {
	int maxid = D->maxid;
	while (a >= maxid || b >= maxid) {
		maxid = maxid * 3 / 2;
	}
	if (maxid > D->maxid) {
		D->h = (struct dirtyhead *)style_realloc(D->C, D->h, D->maxid * sizeof(struct dirtyhead),
			maxid * sizeof(struct dirtyhead));
		int i;
		for (i=D->maxid;i<maxid;i++) {
			D->h[i].head = -1;
			D->h[i].version = 0;
		}
		D->maxid = maxid;
	}
	int index = D->freelist;
	struct dirtyslot * p;
	if (index >= 0) {
		p = &D->p[index];
		D->freelist = p->next;
	} else {
		if (D->n >= D->cap) {
			int cap = D->cap * 3 / 2;
			D->p = (struct dirtyslot *)style_realloc(D->C, D->p, D->cap * sizeof(struct dirtyslot),
				cap * sizeof(struct dirtyslot));
			D->cap = cap;
		}
		index = D->n++;
		p = &D->p[index];
	}
	assert(a >= 0 && b >= 0);
	struct dirtyhead * h = &D->h[a];
	p->version = D->h[b].version;
	p->b = b;
	p->next = h->head;
	h->head = index;
}

void
dirtylist_clear(struct dirtylist *D, int a) {
	assert(a >= 0 && a < D->maxid);
	struct dirtyhead * h = &D->h[a];
	++h->version;
	int index = h->head;
	if (index < 0)
		return;
	int freelist = index;
	h->head = -1;
	for (;;) {
		struct dirtyslot * p = &D->p[index];
		index = p->next;
		if (index < 0) {
			p->next = D->freelist;
			break;
		}
	}
	D->freelist = freelist;
}

static inline int
alive(struct dirtylist *D, struct dirtyslot * p) {
	struct dirtyhead * h = &D->h[p->b];
	return h->version == p->version;
}

int
dirtylist_get(struct dirtylist *D, int id, int n, int *output) {
	assert (id >= 0 || id < D->maxid);
	struct dirtyhead * h = &D->h[id];
	int index = h->head;
	if (index < 0)
		return 0;
	int *list = &h->head;
	int count = 0;
	for (;;) {
		struct dirtyslot * p = &D->p[index];
		if (alive(D, p)) {
			if (count < n) {
				output[count++] = p->b;
			}
			list = &p->next;
		} else {
			*list = p->next;
			p->next = D->freelist;
			D->freelist = index;
		}
		index = *list;
		if (index < 0)
			return count;
	}
}

#include <stdio.h>

void
dirtylist_dump(struct dirtylist *D) {
	int i;
	for (i=0;i<D->maxid;i++) {
		struct dirtyhead * h = &D->h[i];
		int index = h->head;
		if (index >= 0) {
			printf("[%d] : ", i);
			for (;;) {
				struct dirtyslot *p = &D->p[index];
				if (alive(D, p)) {
					printf("%d ", p->b);
				}
				index = p->next;
				if (index < 0)
					break;
			}
			printf("\n");
		}
	}
}

#ifdef DIRTYLIST_TEST_MAIN

#include "style.h"

static void
print_list(struct dirtylist *D, int v) {
	int tmp[4096];
	int i;
	int n = dirtylist_get(D, v, 4096, tmp);
	printf("%d : ", v);
	for (i=0;i<n;i++) {
		printf("%d ", tmp[i]);
	}
	printf("\n");
}

int
main() {
	struct style_cache *C = style_newcache(NULL, NULL, NULL);
	struct dirtylist * D = dirtylist_create(C);

	dirtylist_add(D, 0, 1);	// add 0->1
	dirtylist_add(D, 0, 2);	// add 0->2
	dirtylist_add(D, 0, 3);	// add 0->3

	dirtylist_add(D, 1, 2);	// add 1->2

	dirtylist_add(D, 2, 3);	// add 2->3
	dirtylist_add(D, 2, 0);	// add 2->0

	print_list(D, 0);
	print_list(D, 1);
	print_list(D, 2);
	dirtylist_dump(D);

	dirtylist_clear(D, 2);

	print_list(D, 1);
	print_list(D, 0);
	dirtylist_dump(D);

	dirtylist_release(D);

	return 0;
}

#endif





