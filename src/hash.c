#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/queue.h>

#include "hash.h"

static void h_free_item(h_item_t *item);

h_hash_t *
h_init(int size)
{
	h_hash_t *h = NULL;
	int i = 0;

	if ((h = malloc(sizeof(h_hash_t))) == NULL)
		return NULL;
	if ((h->buckets = malloc(size * sizeof(h_bucket_t))) == NULL)
                return NULL;

	h->size = size;
	h->order_next = NULL;

	for (i = 0; i < size; i++)
		LIST_INIT(&h->buckets[i].head);
	TAILQ_INIT(&h->order_head);

	return h;
}

int
h_remove(h_hash_t *h, char *key)
{
	int hash = 0;
	h_item_t *item = NULL, *item_tmp = NULL;

	hash = h_hash_func(key, strlen(key)) % h->size;
	LIST_FOREACH_SAFE(item, &h->buckets[hash].head, items, item_tmp) {
		if (strcmp(key, item->key) == 0) {
			LIST_REMOVE(item, items);
			TAILQ_REMOVE(&h->order_head, item, ordered_items);
			h_free_item(item);
			return 0;
		}
	}

	return -1;	
}

int
h_insert(h_hash_t *h, char *key, void *value)
{
	h_item_t *item = NULL;
	int hash = 0;

	if ((item = malloc(sizeof(h_item_t))) == NULL)
		return -1;

	//printf("allocing %s and %s\n", key, value);
	item->key = strdup(key);
	item->value = strdup(value);
	hash = h_hash_func(item->key, strlen(item->key)) % h->size;
	LIST_INSERT_HEAD(&h->buckets[hash].head, item, items);
	TAILQ_INSERT_TAIL(&h->order_head, item, ordered_items);

	return 0;
}

void
h_init_traverse(h_hash_t *h)
{
	h->order_next = NULL;
}

void *
h_get(h_hash_t *h, char *key)
{
	int hash = 0;
	h_item_t *item = NULL;

	hash = h_hash_func(key, strlen(key)) % h->size;
	LIST_FOREACH(item, &h->buckets[hash].head, items) {
		if (strcmp(key, item->key) == 0)
			return item->value;
	}

	return NULL;	
}

h_item_t *
h_next(h_hash_t *h)
{
	if (h->order_next == NULL) {
		h->order_next = TAILQ_FIRST(&h->order_head);
	} else {
		h->order_next = TAILQ_NEXT(h->order_next, ordered_items);
	}

	return h->order_next;
}

void
h_free(h_hash_t *h)
{
	h_item_t *item = NULL;
	h_init_traverse(h);
	while ((item = h_next(h)) != NULL) {
		h_free_item(item);
	}
	free(h->buckets);
	free(h);
}

void
h_free_item(h_item_t *item)
{
	free(item->key);
	free(item->value);
	free(item);
}

uint32_t h_hash_func(const char *key, uint32_t len)
{
	int i, hash = 0;

	for (i = 0; i < len; i++) {
		hash += key[i];
	}

	return hash;
}
