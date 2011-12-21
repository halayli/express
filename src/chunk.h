#include <common/mem.h>
#include "http.h"
#include <lthread.h>

mem_pool_t *chunks_pool = NULL;

#define MEM_CHUNKS_INIT(lt, m) \
	do {								\
		m.max_sz = HTTP_MAX_CHUNKS;				\
		m.cur_sz = 0;						\
		if ((lthread_cond_create(lt, &m.rd_lock) == -1) ||	\
		    (lthread_cond_create(lt, &m.wr_lock) == -1)) {	\
			m.wr_lock = m.rd_lock = NULL;			\
			free(m.wr_lock);				\
			free(m.rd_lock);				\
			break;						\
		}							\
		TAILQ_INIT(&m.head);					\
	} while(0)

int
prox_chunks_init(void)
{
/*
	if ((chunks_pool = mem_init_pool(sizeof(mem_chunk_t), 100)) == NULL) {
		perror("Not enough memory to allocate buf_pool");
		return 1;
	}
*/

	return 0;	
}


#define MEM_CHUNKS_FREE(lt, m) 				\
	do {						\
		lthread_cond_signal(lt, m.rd_lock);	\
		lthread_cond_signal(lt, m.wr_lock);	\
		free(m.wr_lock);			\
		free(m.rd_lock);			\
	} while (0)					\

#define CHUNK_ALLOC(chunk)						\
	do {								\
		if ((chunk = malloc(sizeof(mem_chunk_t))) == NULL) {	\
			abort();					\
		}							\
	} while (0)							\

#define CHUNK_DEQUEUE(lt, chunks, chunk)				\
	do {								\
		(chunks).cur_sz -= chunk->sz;				\
		TAILQ_REMOVE(&(chunks).head, chunk, chain);		\
		lthread_cond_signal(lt, (chunks).wr_lock);		\
		free(chunk);						\
	} while(0)							\

#define CHUNK_ENQUEUE(lt, chunks, chunk)				\
	do {								\
		TAILQ_INSERT_TAIL(&(chunks).head, chunk, chain);	\
		(chunks).cur_sz += chunk->sz;				\
		lthread_cond_signal(lt, (chunks).rd_lock);		\
		if ((chunks).cur_sz + chunk->sz > (chunks).max_sz) {	\
			lthread_cond_wait(lt, (chunks).wr_lock);	\
		}							\
	} while (0)							\

#define ALLOW_CHUNK_RD(lt, chunks) \
	lthread_cond_signal(lt, (chunks).rd_lock)

#define ALLOW_CHUNK_WR(lt, chunks) \
	lthread_cond_signal(lt, (chunks).wr_lock)

#define	WAIT_CHUNK_WR(lt, chunks) \
	lthread_cond_wait(lt, (chunks).wr_lock)

#define	WAIT_CHUNK_RD(lt, chunks) \
	lthread_cond_wait(lt, (chunks).rd_lock)
