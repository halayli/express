/*        hash
 * ----------------------
 * |     entry          | ----> item
 * ----------------------
 * |     entry          | ----> item
 * ----------------------
 * |     entry          | ----> item
 * ----------------------
*/
#include <sys/queue.h>
#include <stdint.h>

uint32_t h_hash_func(const char *key, uint32_t len);

struct h_item {
	char *key;
	char *value;
	int order;
	LIST_ENTRY(h_item) items;
	TAILQ_ENTRY(h_item) ordered_items;
};
typedef struct h_item_t_l h_item_t_l;
LIST_HEAD(h_item_t_l, h_item);
TAILQ_HEAD(h_item_t_q, h_item);

struct  h_bucket {
	struct h_item_t_l head;
};

typedef struct h_hash	h_hash_t;
typedef struct h_bucket	h_bucket_t;
typedef struct h_item	h_item_t;

struct h_hash {
	h_bucket_t *buckets;
	struct h_item_t_q order_head;
	int size;
	h_item_t *order_next;
};

h_hash_t *h_init(int size);
int h_remove(h_hash_t *h, char *key);
int h_insert(h_hash_t *h, char *key, void *value);
void h_init_traverse(h_hash_t *h);
h_item_t *h_next(h_hash_t *h);
void h_free(h_hash_t *h);
void *h_get(h_hash_t *h, char *key);
