#include <stdlib.h>
#include <sys/queue.h>
#include <stdint.h>
#include <strings.h>
#include <math.h>
#include "mem.h"

#define QUEUE_MACRO_DEBUG 1
static int mem_realloc_pool(struct mem_pool *m, int size, int n);


void inline
*mem_alloc(struct mem_pool *m)
{
	struct mem_obj *e;
	struct mem_obj  *e_temp;

	if (LIST_EMPTY(&m->l) &&
	    mem_realloc_pool(m, m->obj_size, m->n) == -1) {
		return NULL;
	}

	/* get first free item from the allocated objs */
	LIST_FOREACH_SAFE(e, &m->l, chain, e_temp) {
		LIST_REMOVE(e, chain);
		return e->obj;
	}

	return NULL;
}

struct mem_pool
*mem_init_pool(int size, int n)
{
	struct mem_pool *m;

	if ((m = calloc(1, sizeof (struct mem_pool))) == NULL)
		return NULL;

	m->n = n;
	m->total_n = n;
	m->obj_size = size;

	LIST_INIT(&m->l);

	if (mem_realloc_pool(m, size, n) == -1)
		return NULL;

	return m;
}

int
mem_realloc_pool(struct mem_pool *m, int size, int n)
{
	struct mem_obj *obj;

	m->total_n += n;

	//printf("reallocing %d of size %d\n", n, size);
	while (n--) {
		if ((obj = calloc(1, sizeof (struct mem_obj))) == NULL)
			goto err;
		if ((obj->obj = calloc(1, size + sizeof(struct mem_obj*))) == NULL)
			goto err;
		obj->m = m;
		((void **)obj->obj)[0] = obj;
		obj->obj += sizeof (void *);
		LIST_INSERT_HEAD(&m->l, obj, chain);
	}
	//printf("reallocc!: size %d  n : %d\n", size, n);

	return 0;

err:
	mem_free_pool(m);
	return -1;
}

void
mem_free_pool(struct mem_pool *m)
{
	struct mem_obj *obj;
	struct mem_obj *obj_temp;

	LIST_FOREACH_SAFE(obj, &m->l, chain, obj_temp) {
		obj->obj -= sizeof (void *);
		free(obj->obj);
		LIST_REMOVE(obj, chain);
		free(obj);
	}
}
