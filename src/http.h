#ifndef _HTTP_H_
#define _HTTP_H_

#include <sys/queue.h>
#include <lthread.h>
#include "http_str.h"
#include "log.h"
#include "hash.h"
#include "common/mem.h"

#define	TCP_BUF_SIZE		(32*1024)
#define HTTP_MAX_HDR_LEN        (8192)
#define HTTP_MAX_HDRS		(64)
#define HTTP_MAX_CHUNKS        (TCP_BUF_SIZE * 5)
#define HTTP_MAX_HOST_LEN            (255)
#define HTTP_MAX_REQ_LINE_LEN   (4096)
#define HTTP_DEFAULT_SERVER_PORT (80)

#define NELEMENTS(x) (sizeof (x) / sizeof x[0])
#define IS_SET(x, y)    ((x) & (1<<(y)))
#define SET_BIT(x, y)   ((x) |= (1<<(y)))


typedef enum {
        ST_TRANS_INIT = 0,
        ST_TRANS_START,
        ST_TRANS_IN_PROG,
        ST_TRANS_IN_PROG1,
        ST_TRANS_COMPLETE,
        ST_TRANS_EXITED,
} st_trans_t;

typedef enum {
        HTTP_GET = 1,
        HTTP_POST,
        HTTP_PUT,
        HTTP_HEAD,
        HTTP_DELETE,
} http_method_t;

typedef struct lsn         	lsn_t;
typedef struct http_prox        http_prox_t;
typedef struct http_srv         http_srv_t;
typedef struct http_cli         http_cli_t;
typedef struct http_hdr         http_hdr_t;
typedef struct http_req         http_req_t;
typedef struct http_resp        http_resp_t;
typedef struct http_sess        http_sess_t;
typedef struct http_conn        http_conn_t;
typedef struct mem_chunk 	mem_chunk_t;
typedef struct mem_chunks 	mem_chunks_t;

typedef struct http_srv_t_l	http_srv_t_l;
typedef struct http_prox_t_l	http_prox_t_l;
typedef struct mem_chunk_t_l	mem_chunk_t_l;

TAILQ_HEAD(mem_chunk_t_l, mem_chunk);
LIST_HEAD(http_srv_t_l, http_srv);
LIST_HEAD(http_prox_t_l, http_prox);

typedef enum {
	HTTP_504,
	HTTP_502,
	HTTP_200,
	HTTP_400,
} http_code_t;

typedef enum  {
        HTTP_FAIL = -1,
	HTTP_ERR = 1,
        HTTP_ERR_UNKNOWN_HOST,	/* no host in url or host hdr   */
        HTTP_ERR_NO_CNT_LEN,	/* no content length            */
        HTTP_ERR_MAX_HDR_EXC,	/* max hdr size exceeded        */
        HTTP_ERR_CLI_CLOSED,	/* cli closed connection        */
        HTTP_ERR_INV_REQ_LINE,	/* invalid request line         */
        HTTP_ERR_INV_RESP_CODE,	/* invalid response code        */
        HTTP_ERR_INV_METHOD,	/* invalid method               */
        HTTP_ERR_INV_RESP_LINE,	/* improper response line       */
        HTTP_ERR_INV_HOST,	/* invalid host format          */
        HTTP_ERR_INV_PORT,	/* port out of range            */
        HTTP_ERR_INV_PROTO,	/* invalid protocol             */
	HTTP_ERR_TIMEOUT_EXC,	/* read/write timeout exceeded */
} http_err_t;

struct mem_chunk {
	char _buf[TCP_BUF_SIZE];
	uint32_t sz;
	TAILQ_ENTRY(mem_chunk) chain;
};

struct mem_chunks {
	uint32_t 	max_sz;
	uint32_t 	cur_sz;
	mem_chunk_t_l 	head;
	lthread_cond_t *rd_lock;
	lthread_cond_t *wr_lock;
};

struct http_hdr {
	char 		hdr[HTTP_MAX_HDR_LEN];
	int 		hdr_start;
	int		hdr_len;
	uint64_t 	cnt_len;   /* content-length or chunk size */
	int		cnt_recvd; /* len of data recvd after the hdr */
	unsigned	chunked:1;
	unsigned	nolen:1;
	unsigned	keepalive:1;
	unsigned	http11:1;
	h_hash_t	*hdrs;
};

/*
 * Request Structure
*/
struct http_req {
	char		*host;
	http_method_t	method;
	char 		*method_str;
	char		*req_line;
	char		*uri;
	unsigned short	port; /* port if any (default 80) */
	unsigned	expect100:1;
};

/*
 * Response Structure
*/
struct http_resp {
	char 		*status_line;
	int		resp_code;
	int		resp_code_str;
};

/*
 * Connection Structure
*/
struct http_conn {
	int		fd;
	struct in_addr	peer_addr;
	unsigned short	peer_port; /* ephem port*/
	struct in_addr	srv_addr;
	unsigned short	srv_port; /* ephem port*/
	char		*srv_host;
	unsigned	shutdown_rd:1;
	unsigned	shutdown_wr:1;
};

/*
 * Session Structure
*/
struct http_sess {
	http_hdr_t	hdr;
	char		*buf;
	http_conn_t	conn;
};

/*
 * Server Structure
*/
struct http_srv {
	http_sess_t	sess;
	http_resp_t	resp;
	int 		ref;
	LIST_ENTRY(http_srv)	chain;
	st_trans_t	st;
};

/*
 * Client Structure
*/
struct http_cli {
	http_sess_t	sess;
	http_req_t	req;
	st_trans_t	st;
};

/*
 * Proxy Structure
*/
struct http_prox {
	http_srv_t_l	srvs[20]; /* List of established server sessions */
	http_srv_t	*srv;  /* active server connection */
	http_cli_t	cli;  /* established client session */
	lsn_t		*lsn;
	int		ref;
	mem_chunks_t	cli_buf;
	mem_chunks_t	srv_buf;
	LIST_ENTRY(http_prox) chain;
};

/*
 * Global Proxy Structure
*/
struct lsn {
	/* preallocated pools */
	mem_pool_t 	*prox_pool;
	mem_pool_t 	*srv_pool;
	http_prox_t_l	active;
	unsigned short	lsn_port;
	struct in_addr	lsn_addr;
	sched_t		*sched;
	int		prox_log_level;
	prox_log_t 	http_log;
	prox_log_t 	prox_log;
	uint64_t	birth;
	void		*js_context;
};

mem_chunk_t* mem_chunk_create();
void mem_chunk_free();
int mem_chunks_init(lthread_t *lt, mem_chunks_t*);

/*
 * from http_conn.c
 */
int 	http_send(lthread_t *lt, http_sess_t *c, char *buf, uint32_t len);
int 	http_recv(lthread_t *lt, http_sess_t *c, char *buf, uint32_t len);
int	http_connect(lthread_t *lt, http_prox_t *prox);
int	http_save_c_conn_info(int fd, http_conn_t *conn);
int	http_save_s_conn_info(int fd, http_conn_t *conn);
http_srv_t *http_select_srv(http_prox_t *prox);

/*
 * from http_parser.c
 */
int 	http_parse_req_line(http_cli_t *c);
int 	http_parse_resp_line(http_srv_t *s);
int 	http_parse_resp_hdr(http_srv_t *s);
int 	http_parse_req_hdr(http_cli_t *c);
int 	http_parse_hdr(http_sess_t *sess);

#endif
