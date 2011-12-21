#ifndef _MEM_H_
#define _MEM_H_

#include <sys/queue.h>
struct mem_pool;

struct mem_obj {
	void			*obj;
	LIST_ENTRY(mem_obj) 	chain;
	struct mem_pool		*m;
};

LIST_HEAD(mem_obj_l, mem_obj);

struct mem_pool {
	struct mem_obj_l 	l;		/* list of mem_obj */
	uint64_t		obj_size;	/* obj size */
	uint64_t		n;		/* total objs in pool */
	uint64_t		total_n;	/* total objs in pool */
};


typedef struct mem_pool mem_pool_t;

/************************************************************************
 * pool prototypes                                                  *
 ***********************************************************************/
mem_pool_t	*mem_init_pool(int size, int n);
void		mem_free_pool(mem_pool_t *m);
void		*mem_alloc(mem_pool_t *);
void		mem_free(void *o);

#define MEM_FREE(x) 						\
	do {							\
		if (x == NULL) 					\
			break; 					\
		struct mem_obj *obj = ((void **)x)[-1]; 	\
		LIST_INSERT_HEAD(&obj->m->l, obj, chain); 	\
	} while (0)						\

#endif
