#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/queue.h>
#include <signal.h>
#include <string.h>
#include <err.h>
#include <sys/resource.h>

#include <arpa/inet.h>
#include <netdb.h>

#include <sys/rtprio.h>

#include "http_str.h"
#include "http.h"
#include "prox_bd.h"
#include "sock_easy.h"
#include "log.h"
#include <lthread.h>
#include "common/mem.h"
#include "common/time.h"
#include "chunk.h"

int total = 0;
char log_buf[8192] = {0};

/*
 *                +---------+      +-------+      +--------+                  
 *                | cli_rd  |----->|cli_buf|----->| srv_wr |                  
 *                +---------+      +-------+      +--------+                  
                  ^                                         \
 *               /                                           \                
 * +---------+  /                                             >  +---------+  
 * | browser | /                                                 | web srv |  
 * +---------+                                                   +---------+  
              ^                                                 /
 *             \                                               /              
 *              \ +---------+      +-------+      +--------+  /               
 *               \| cli_wr  |<-----|srv_buf|<-----| srv_rd | <                
 *                +---------+      +-------+      +--------+                  
 *
 *
 * XXX: write down a state diagram of a cli & srv connection life
 */

/************************************************************************
 * http proxy prototypes                                                *
 ***********************************************************************/
static int 	http_recv_hdr(lthread_t *lt, http_sess_t *sess);
static int 	http_recv_resp_hdr(lthread_t *lt, http_srv_t *s);
static int 	http_recv_req_hdr(lthread_t *lt, http_cli_t *c);

static int	http_passthrough_exact(lthread_t *lt, http_prox_t *prox,
		    http_sess_t *from, http_sess_t *to,int dir);
static int	http_passthrough_chunked(lthread_t *lt, http_prox_t *prox,
		    http_sess_t *from, http_sess_t *to, int dir);
static int	http_passthrough(lthread_t *lt, http_prox_t *prox,
		    http_sess_t *from, http_sess_t *to, int dir);

static int	http_handle_srv_rd(lthread_t *lt, http_prox_t *prox);
static int	http_handle_cli_rd(lthread_t *lt, http_prox_t *prox);
static void	http_lt_cli_rd(lthread_t *lt, http_prox_t *prox);
static void	http_lt_cli_wr(lthread_t *lt, http_prox_t *prox);
static void	http_lt_srv_rd(lthread_t *lt, http_prox_t *prox);
static void	http_lt_srv_wr(lthread_t *lt, http_prox_t *prox);
static void	http_listener(lthread_t *lt, lsn_t *lsn);
static void	http_cli_new(lthread_t *lt, lsn_t *lsn, int c);

static int 	wr_chunks(lthread_t *lt, http_prox_t *prox, http_sess_t *sess,
    mem_chunks_t *chunks);

static int	http_cli_init(lthread_t *lt, http_prox_t *prox);
static void	http_cli_free(lthread_t *lt, http_prox_t *prox);
static void	http_srv_free(lthread_t *lt, http_prox_t *prox,
		    http_srv_t *srv);
static void	http_respond(lthread_t *lt, http_prox_t *prox, int);

static void 	http_log_request(http_prox_t *prox);
static char 	*http_get_method_str(http_method_t m);

static int 	http_send_cli_hdr(lthread_t *lt, http_cli_t *c, http_srv_t *s);

static void	shutdown_rd(http_conn_t *conn);
static void	shutdown_wr(http_conn_t *conn);

static void
http_respond(lthread_t *lt, http_prox_t *prox, int code)
{
	char *msg = NULL;

	char *http_504 = "HTTP/1.0 504 Server Not Found\r\n"
	    "Cache-Control: no-cache\r\n"
	    "Connection: close\r\n"
	    "Content-Type: text/html\r\n"
	    "\r\n"
	    "<html><body><h1>504 Gateway Time-out</h1>\n"
	    "The server didn't respond in time.\n</body></html>\n";

	char *http_400 = "HTTP/1.0 400 Bad Request\r\n"
	    "Cache-Control: no-cache\r\n"
	    "Connection: close\r\n"
	    "Content-Type: text/html\r\n"
	    "\r\n"
	    "<html><body><h1>400 Bad Request</h1>\n"
	    "Failed to process http request.\n</body></html>\n";

	switch (code) {
	case HTTP_400:
		msg = http_400;
		break;
	case HTTP_504:
		msg = http_504;
		break;
	default:
		msg = NULL;
	
	}

	if (msg)
		http_send(lt, &prox->cli.sess, msg, strlen(msg));
}

static int
http_send_cli_hdr(lthread_t *lt, http_cli_t *c, http_srv_t *s)
{
	int total_length = 0, start = 0, ret = 0;
	h_item_t *item = NULL;
	char *buf = NULL;

	total_length = snprintf(NULL, 0, "%s %s %s\n", c->req.method_str, c->req.uri,
	    c->sess.hdr.http11 == 1 ? "HTTP/1.1":"HTTP/1.0");
	while ((item = h_next(c->sess.hdr.hdrs)) != NULL) {
		total_length += strlen(item->key);
		total_length += strlen(item->value);
		total_length += 3; // :<sp> \n 
	}

	total_length += 2; // trailing \n and \0 if we need to print it

	if ((buf = malloc(total_length)) == NULL)
		return -1;

	start = sprintf(buf, "%s %s %s\n", c->req.method_str, c->req.uri,
	    c->sess.hdr.http11 == 1 ? "HTTP/1.1":"HTTP/1.0");

	while ((item = h_next(c->sess.hdr.hdrs)) != NULL) {
		start += sprintf(&buf[start], "%s: %s\n", item->key, item->value);
	}

	buf[start] = '\n';
	buf[++start] = '\0';
	//printf("buf is %d and length of header is %d\n", total_length, strlen(buf));
	//printf("cli:buf is %d and length of header is %d, start is %d\n", total_length, strlen(buf), start);
	//http_print_exact(buf, strlen(buf));
	/*printf("cli: buf is %d and length of header is %d, start is %d\nheader:\n%s",
		total_length, strlen(buf), start, buf);*/
	
	ret = lthread_send(lt, s->sess.conn.fd, buf, start, 0);
	free(buf);

	return ret;
}

static int
http_send_srv_hdr(lthread_t *lt, http_srv_t *s, http_cli_t *c)
{
	int total_length = 0, start = 0, ret = 0;
	h_item_t *item = NULL;
	char *buf = NULL;

	total_length = snprintf(NULL, 0, "%s %d %s\n",
	    s->sess.hdr.http11 == 1 ? "HTTP/1.1":"HTTP/1.0", s->resp.resp_code,
	    &s->sess.hdr.hdr[s->resp.resp_code_str]);
	while ((item = h_next(s->sess.hdr.hdrs)) != NULL) {
		total_length += strlen(item->key);
		total_length += strlen(item->value);
		total_length += 3; // :<sp> \n 
	}

	total_length += 2; // trailing \n and \0 if we need to print it

	if ((buf = malloc(total_length + s->sess.hdr.cnt_recvd)) == NULL)
		return -1;

	start = sprintf(buf, "%s %d %s\n",
	    s->sess.hdr.http11 == 1 ? "HTTP/1.1":"HTTP/1.0", s->resp.resp_code,
	    &s->sess.hdr.hdr[s->resp.resp_code_str]);

	while ((item = h_next(s->sess.hdr.hdrs)) != NULL) {
		start += sprintf(&buf[start],
			"%s: %s\n", item->key, item->value);
	}

	buf[start++] = '\n';
	memmove(&buf[start],&s->sess.hdr.hdr[s->sess.hdr.hdr_len], s->sess.hdr.cnt_recvd);
	start += s->sess.hdr.cnt_recvd;
	/*start += snprintf(&buf[start], s->sess.hdr.cnt_recvd,
	    "\n%s", &s->sess.hdr.hdr[s->sess.hdr.hdr_len]);*/
	/*printf("srv: buf is %d and length of header is %d, start is %d\nheader:\n%s",
		total_length, strlen(buf), start, buf);*/
	
	ret = lthread_send(lt, c->sess.conn.fd, buf, start, 0);
	free(buf);

	return 0;
}

static int
http_recv_hdr(lthread_t *lt, http_sess_t *sess)
{
	int recvd = 0;
	int hdr_len = 0;
	int ret = 0;
	char *hdr_end;
	sess->hdr.cnt_len = 0;
	sess->hdr.cnt_recvd = 0;

	while ((HTTP_MAX_HDR_LEN - recvd) > 0) {
		ret = http_recv(lt, sess, &sess->hdr.hdr[recvd],
		    HTTP_MAX_HDR_LEN - recvd);

		if (ret == -2)
			return HTTP_ERR_TIMEOUT_EXC;
		if (ret < 1)
			return HTTP_ERR_CLI_CLOSED;

		recvd += ret;
		if ((hdr_end =
		    strnstr(sess->hdr.hdr, "\r\n\r\n", recvd)) ||
		   (hdr_end = strnstr(sess->hdr.hdr, "\n\n", recvd))) {

			hdr_len = hdr_end - sess->hdr.hdr;
			/* check which whitespace we matched and advance */
			hdr_len += (hdr_end[0] == '\r') ? 4 : 2;

			sess->hdr.hdr_len = hdr_len;
			sess->hdr.cnt_recvd = recvd - hdr_len;

			return 0;
		}
	}

	return HTTP_ERR_MAX_HDR_EXC;
}

static int
http_recv_resp_hdr(lthread_t *lt, http_srv_t *s)
{
	int err = 0;
	err = http_recv_hdr(lt, &s->sess);

	if (err != 0)
		return err;

	err = http_parse_resp_hdr(s);

	return err;
}

static int 
http_recv_req_hdr(lthread_t *lt, http_cli_t *c)
{
	int err = 0;
	err = http_recv_hdr(lt, &c->sess);

	if (err != 0)
		return err;

	err = http_parse_req_hdr(c);

	return err;
}

static int
http_cli_init(lthread_t *lt, http_prox_t *prox)
{
	size_t i = 0;
	prox->srv = NULL;
	prox->ref = 0;
	prox->cli.sess.conn.shutdown_rd = 0;
	prox->cli.sess.conn.shutdown_wr = 0;
	if ((prox->cli.sess.hdr.hdrs = h_init(128)) == NULL)
		return -1;

	for (i = 0; i < NELEMENTS(prox->srvs); i++)
		LIST_INIT(&prox->srvs[i]);

	MEM_CHUNKS_INIT(lt, prox->cli_buf);
	MEM_CHUNKS_INIT(lt, prox->srv_buf);
	if (prox->cli_buf.rd_lock == NULL || prox->srv_buf.rd_lock == NULL)
		return -1;

	return 0;
}

static void
http_cli_free(lthread_t *lt, http_prox_t *prox)
{
	http_srv_t *s = NULL;
	size_t i = 0;

	prox->ref--;

	ALLOW_CHUNK_RD(lt, prox->srv_buf);
	shutdown_rd(&prox->cli.sess.conn);
	shutdown_wr(&prox->cli.sess.conn);

	for (i = 0; i < NELEMENTS(prox->srvs); i++) {
		LIST_FOREACH(s, &prox->srvs[i], chain) {
			shutdown(s->sess.conn.fd, SHUT_RDWR);
		}
	}

	if (prox->ref == 0) {
		LIST_REMOVE(prox, chain);
		total--;
		lthread_close(lt, prox->cli.sess.conn.fd);
		MEM_CHUNKS_FREE(lt, prox->cli_buf);
		MEM_CHUNKS_FREE(lt, prox->srv_buf);
		free(prox->cli.req.uri);
		free(prox->cli.req.host);
		h_free(prox->cli.sess.hdr.hdrs);
		free(prox);
	} else if (prox->ref < 0) {
		prox_log(lt, prox, PROX_ERR, PROX_GEN_FAIL,
		    "prox ref < 0. Impossible State!");
	}

}

static void
http_srv_free(lthread_t *lt, http_prox_t *prox, http_srv_t *srv)
{
	srv->ref--;
	prox->ref--;

	/* let the cli_wr waiting to write to client unlock and exit. */
	ALLOW_CHUNK_RD(lt, prox->srv_buf);

	shutdown_rd(&srv->sess.conn);
	shutdown_wr(&srv->sess.conn);

	if (srv->ref == 0) {
		lthread_close(lt, srv->sess.conn.fd);
		LIST_REMOVE(srv, chain);
		free(srv->sess.conn.srv_host);
		h_free(srv->sess.hdr.hdrs);
		free(srv);
	}

	if (prox->ref == 0) {
		prox->ref++;
		http_cli_free(lt, prox);
	}
}

static void
shutdown_wr(http_conn_t *conn)
{
	if (conn->shutdown_wr == 0) {
		shutdown(conn->fd, SHUT_WR);
		conn->shutdown_wr = 1;
	}
}

static void
shutdown_rd(http_conn_t *conn)
{
	if (conn->shutdown_rd == 0) {
		shutdown(conn->fd, SHUT_RD);
		conn->shutdown_rd = 1;
	}
}

/* write data to the client */
static void
http_lt_cli_wr(lthread_t *lt, http_prox_t *prox)
{

	mem_chunk_t *buf = NULL, *tbuf = NULL;
	int ret = 0;
	prox->ref++;

	DEFINE_LTHREAD(lt);

	prox_log(lt, prox, PROX_TRC, PROX_CLI_WR_START);
	while (1) {
		prox_log(lt, prox, PROX_TRC, PROX_CLI_WR_WAIT);
		ret = wr_chunks(lt, prox, &prox->cli.sess, &prox->srv_buf);
		prox_log(lt, prox, PROX_TRC, PROX_CLI_WR_WAIT_DONE);
		if (ret == -1)
			break;
	}

	TAILQ_FOREACH_SAFE (buf, &prox->srv_buf.head, chain, tbuf) {
		CHUNK_DEQUEUE(lt, prox->srv_buf, buf);
	}

	shutdown_wr(&prox->cli.sess.conn);
	http_cli_free(lt, prox);
	prox_log(lt, prox, PROX_TRC, PROX_CLI_WR_DONE);
}

/* write data to server */
static void
http_lt_srv_wr(lthread_t *lt, http_prox_t *prox)
{
	mem_chunk_t *buf = NULL, *tbuf = NULL;
	int ret = 0;
	http_srv_t **s = &prox->srv;
	DEFINE_LTHREAD(lt);

	prox_log(lt, prox, PROX_TRC, PROX_SRV_WR_START);
	while (1) {
		prox_log(lt, prox, PROX_TRC, PROX_SRV_WR_WAIT);
		ret = wr_chunks(lt, prox, &prox->srv->sess, &prox->cli_buf);
		prox_log(lt, prox, PROX_TRC, PROX_SRV_WR_WAIT_DONE);
		if (ret == -1)
			break;
	}

	TAILQ_FOREACH_SAFE(buf, &prox->cli_buf.head, chain, tbuf) {
		CHUNK_DEQUEUE(lt, prox->cli_buf, buf);
	}

	shutdown_wr(&(*s)->sess.conn);
	http_srv_free(lt, prox, *s);
	prox_log(lt, prox, PROX_TRC, PROX_SRV_WR_DONE);
}

/* write chunks to client or server depending on the session passed */
static int
wr_chunks(lthread_t *lt, http_prox_t *prox, http_sess_t *sess,
    mem_chunks_t *chunks)
{
	mem_chunk_t *buf = NULL, *tbuf = NULL;
	int ret = 0;

	/* wait until there is a chunk to send back to the client */
	WAIT_CHUNK_RD(lt, *chunks);

	while (!TAILQ_EMPTY(&chunks->head)) {
		TAILQ_FOREACH_SAFE(buf, &chunks->head, chain, tbuf) {
			ret = http_send(lt, sess, buf->_buf, buf->sz);
			if (ret <= 0) {
				CHUNK_DEQUEUE(lt, *chunks, buf);
				shutdown_rd(&prox->cli.sess.conn);
				return -1;
			}

			CHUNK_DEQUEUE(lt, *chunks, buf);
		}
	}

	if (prox->cli.st == ST_TRANS_IN_PROG1)
		prox->cli.st = ST_TRANS_START;

	if (prox->cli.st == ST_TRANS_EXITED) {
		shutdown_rd(&prox->cli.sess.conn);
		return -1;
	}

	return 0;
}

static void
http_handle_cli_req_err(lthread_t *lt, http_prox_t *prox, int err)
{
	switch(err) {
	case HTTP_ERR_UNKNOWN_HOST:
	case HTTP_ERR_NO_CNT_LEN:
	case HTTP_ERR_INV_REQ_LINE:
	case HTTP_ERR_MAX_HDR_EXC:
	case HTTP_ERR_INV_HOST:
	case HTTP_ERR_INV_PORT:
	case HTTP_ERR_INV_PROTO:
		http_respond(lt, prox, HTTP_400);
		break;
	}

	switch(err) {
	case HTTP_ERR_UNKNOWN_HOST:
		prox_log(lt, prox, PROX_TRC, PROX_HTTP_GEN_FAIL,
		    "invalid request. Unknown host.");
		break;
	case HTTP_ERR_NO_CNT_LEN:
		prox_log(lt, prox, PROX_TRC, PROX_HTTP_GEN_FAIL,
		    "invalid request. no cnt len.");
		break;
	case HTTP_ERR_INV_REQ_LINE:
		prox_log(lt, prox, PROX_TRC, PROX_HTTP_GEN_FAIL,
		    "invalid request. invalid request line.");
		break;
	case HTTP_ERR_MAX_HDR_EXC:
		prox_log(lt, prox, PROX_TRC, PROX_HTTP_GEN_FAIL,
		    "invalid request. max hdr exceeded.");
		break;
	case HTTP_ERR_INV_HOST:
		prox_log(lt, prox, PROX_TRC, PROX_HTTP_GEN_FAIL,
		    "invalid request. invalid host.");
		break;
	case HTTP_ERR_INV_PORT:
		prox_log(lt, prox, PROX_TRC, PROX_HTTP_GEN_FAIL,
		    "invalid request. invalid port.");
		break;
	case HTTP_ERR_INV_PROTO:
		prox_log(lt, prox, PROX_TRC, PROX_HTTP_GEN_FAIL,
		    "invalid request. invalid protocol.");
		break;
	case HTTP_ERR_TIMEOUT_EXC:
		prox_log(lt, prox, PROX_TRC, PROX_HTTP_GEN_FAIL,
		    "connection timed out.");
		break;
	}
}

/* handles one cli request in each call */
static int
http_handle_cli_rd(lthread_t *lt, http_prox_t *prox)
{
	sched_t *sched = lthread_get_sched(lt);
	int err = 0;
	http_cli_t *c = NULL;
	http_srv_t *s = NULL;
	lthread_t *lt_new = NULL;
	int ret = 0;

	c = &prox->cli;

	/* recv client hdr after validating it. */
	if ((err = http_recv_req_hdr(lt, c)) != 0) {
		http_handle_cli_req_err(lt, prox, err);
		return -1;
	}

	/* 
	 * XXX: hand over request to JS.
	 * Process the JS code, if it allowed us to resume, we'll proceed
	 * further, else, we'll need to [respond] and drop connection.
	 * JS code will be allowed to do the following:
	 * 1. complete access to headers (read/write)
	 * 2. modify action (deny, allow, respond, cache, nocache). In case of
	 * deny, connection gets dropped. respond will get an option to drop
         * connection or keep it open.
	 */

	if (c->st != ST_TRANS_START) {
		prox_log(lt, prox, PROX_WRN, PROX_CLI_EARLY_REQ);
		return -1;
	}

	prox_log(lt, prox, PROX_DBG, PROX_CLI_REQ);

	if ((s = http_select_srv(prox)) != NULL) {
		/* found an already established connection */
		prox->srv = s;
	} else {
		err = http_connect(lt, prox);
		if (err == -1) {
			http_respond(lt, prox, HTTP_504);
			return -1;
		}
		prox->ref++;
		prox->srv->ref++;
		ret = lthread_create(sched, &lt_new, http_lt_srv_rd, prox);
		if (ret != 0) {
			prox_log(lt, prox, PROX_ERR, PROX_LT_FAIL,
			    "http_handle_cli_rd");
			return -1;
		}
	}

	/* 
	 * we established connection with server, launched thread, and now
	 * it is time to send a request to the server. http_handle_cli_rd
	 * sends the request only, to the server, when dealing with
	 * posts, it reads from the cli and sends to a buffer.
	 */
	prox->srv->st = ST_TRANS_START;

	if (c->req.method == HTTP_POST) {
		prox->ref++;
		prox->srv->ref++;
		/* XXX: this won't die when unlocking. fix */
		ret = lthread_create(sched, &lt_new, http_lt_srv_wr, prox);
		if (ret != 0) {
			prox_log(lt, prox, PROX_ERR, PROX_LT_FAIL,
			    "http_handle_cli_rd");
			return -1;
		}

		err = http_passthrough(lt, prox, &c->sess, &prox->srv->sess, 1);
		if (err)
			prox_log(lt, prox, PROX_TRC, PROX_CONN_FAIL);
		/* 
		 * we read everything from the cli, send it to buffer for the
		 * server to consume it. Once it is all consumed, srv_wr 
		 * will change the cli status to ST_TRANS_START.
		 * This is to avoid receiving another request from cli before
		 * having finished sending the data to the server.
		 */
		prox->cli.st = ST_TRANS_IN_PROG1;
		/* allow srv_wr to exit and restore cli status */
		ALLOW_CHUNK_RD(lt, prox->cli_buf);
	} else {
		prox->cli.st = ST_TRANS_IN_PROG;
		/* send the client request to the web server */
		err = http_send_cli_hdr(lt, &prox->cli, prox->srv);
		if (err == -1) {
			http_respond(lt, prox, HTTP_504);
			shutdown_rd(&prox->cli.sess.conn);
			prox->srv->st = ST_TRANS_EXITED;
			ALLOW_CHUNK_RD(lt, prox->srv_buf);
			return -1;
		}
	}

	return 0;
}

static int
http_handle_srv_rd(lthread_t *lt, http_prox_t *prox)
{
	int err = 0;
	http_cli_t *c = NULL;
	http_srv_t *s = prox->srv;
	c = &prox->cli;

	/* 
	 * get response from server. if server died here, send a 502
	 * and assume we got an invalid response. Only respond back to the
	 * user if the user is expecting a response.
	 */
	if ((err = http_recv_resp_hdr(lt, s)) != 0) {
		/* XXX: log a timeout */
		if (s->st == ST_TRANS_START)
			http_respond(lt, prox, HTTP_502);
		shutdown_rd(&prox->cli.sess.conn);
		return -1;
	}

	if (s->st != ST_TRANS_START) {
		prox_log(lt, prox, PROX_WRN, PROX_SRV_EARLY_RESP);
		return -1;
	}

	prox_log(lt, prox, PROX_DBG, PROX_SRV_RESP);

	/* pass the data from server -> client */
	err = http_passthrough(lt, prox, &s->sess, &c->sess, 0);
	if (err == 0)
		http_log_request(prox);

	prox_log(lt, prox, PROX_TRC, PROX_PASSTHRU, err);

	/* 
	 * Closing connection on the client after the server closed
	 * on us is the only way to tell the client that the server
	 * is done.
	 */
	if (err != 0 || c->sess.hdr.http11 == 0 || s->sess.hdr.http11 == 0) {
		prox->cli.st = ST_TRANS_EXITED;
		ALLOW_CHUNK_RD(lt, prox->srv_buf);
		shutdown_rd(&prox->cli.sess.conn);
		return -1;
	}

	s->st = ST_TRANS_COMPLETE;
	prox->cli.st = ST_TRANS_IN_PROG1;
	/* allow cli_wr to unlock to restore the cli status to ST_TRANS_START */
	ALLOW_CHUNK_RD(lt, prox->srv_buf);

	return 0;
}

/* read data from client */
static void
http_lt_cli_rd(lthread_t *lt, http_prox_t *prox)
{
	int err = 0;
	DEFINE_LTHREAD(lt);

	/* this thread is using prox, increase ref */
	prox->ref++;
	prox->cli.st = ST_TRANS_START;
	prox_log(lt, prox, PROX_TRC, PROX_CLI_CONN,
	    inet_ntoa(prox->cli.sess.conn.peer_addr),
	    prox->cli.sess.conn.peer_port);

	while (1) {
		err = http_handle_cli_rd(lt, prox);
		if (err == -1)
			break;
	}

	/* clean up client/server(s) conns and exit */
	prox_log(lt, prox, PROX_TRC, PROX_CLI_DISC,
	    inet_ntoa(prox->cli.sess.conn.peer_addr),
	    prox->cli.sess.conn.peer_port);
	prox->cli.st = ST_TRANS_EXITED;
	shutdown_rd(&prox->cli.sess.conn);
	http_cli_free(lt, prox);
}

/* read data from server */
static void
http_lt_srv_rd(lthread_t *lt, http_prox_t *prox)
{
	int err = 0;

	http_srv_t *s = prox->srv;
	DEFINE_LTHREAD(lt);

	prox_log(lt, prox, PROX_TRC, PROX_SRV_CONN, s->sess.conn.srv_host);

	while (1) {
		err = http_handle_srv_rd(lt, prox);
		if (err == -1) {
			break;
		}
	}

	prox_log(lt, prox, PROX_TRC, PROX_SRV_DISC, s->sess.conn.srv_host);
	shutdown_rd(&s->sess.conn);
	http_srv_free(lt, prox, s);
}

static char *
http_get_method_str(http_method_t m)
{
	switch(m) {
		case HTTP_GET:
			return "GET";
		case HTTP_POST:
			return "POST";
		case HTTP_DELETE:
			return "DELETE";
		case HTTP_PUT:
			return "PUT";
		case HTTP_HEAD:
			return "HEAD";
	}
	return "Unknown Method";
}

static void
http_log_request(http_prox_t *prox)
{
	sprintf(log_buf, "%s %s %s %s %s [%s]",
	    inet_ntoa(prox->cli.sess.conn.peer_addr),
	    prox->srv->sess.conn.srv_host,
	    http_get_method_str(prox->cli.req.method),
	    prox->cli.req.uri,
	    prox->cli.sess.hdr.http11 ? "HTTP/1.1" : "HTTP/1.0 ",
	    (char *)h_get(prox->cli.sess.hdr.hdrs, "user-agent")
	);

	printf("%s\n", log_buf);
}

static int
http_passthrough_exact(lthread_t *lt, http_prox_t *prox,
    http_sess_t *from, http_sess_t *to, int dir)
{
	uint64_t recvd = 0; 
	uint64_t ret = 0;
	int send_until_close = 0;
	mem_chunk_t *buf = NULL;
	mem_chunks_t *dir_buf = (dir) ? &prox->cli_buf : &prox->srv_buf;
	int err = 0;

	if ((err = http_send_srv_hdr(lt, prox->srv, &prox->cli)) == -1)
		goto done;

	if (from->hdr.nolen == 1)
		send_until_close = 1;


	recvd = from->hdr.cnt_recvd;
	if (recvd == from->hdr.cnt_len && !send_until_close) {
		err = 0;
		goto done;
	}

	/* 
	 * Stupid webservers implemented by monkeys don't send content-length
	 * header. Send until socket is closed.
	 */
	while (recvd < from->hdr.cnt_len || send_until_close) {
		CHUNK_ALLOC(buf);
		if (buf == NULL) {
			prox_log(lt, prox, PROX_ERR, PROX_MEM_FAIL,
			    "chunk_alloc failed");
			err = -1;
			goto done;
		}
		ret = http_recv(lt, from, buf->_buf, TCP_BUF_SIZE);
		buf->sz = ret;
		/*
		 * enqueue the chunk to be written, if we are running out of
		 * total chunks / transaction, then we'll block until it gets
		 * consumed by the other end. This happens when one end is
		 * consuming at a lower speed than the data that is coming in.
		 */

		if (ret == 0) {
			/* it is not an error if ret == 0 and we are not
			 * provided a content-length, i.e, send_until_close is 1
			 * but we return 1 to tell the caller to close the 
			 * connection.
			 */
			err = (send_until_close == 1);
			goto done;
		}

		if (ret == -1) {
			err = -1;
			goto done;
		}

		CHUNK_ENQUEUE(lt, *dir_buf, buf);
		recvd += ret;
	}

	err = 0;

done:
	return err;

}

static int
http_passthrough_chunked(lthread_t *lt, http_prox_t *prox, http_sess_t *from,
    http_sess_t *to, int dir)
{
	int64_t recvd = 0;
	int64_t len = 0;
	int64_t more = 0;
	int64_t ret = 0;
	int send_until_close = 0;
	mem_chunk_t *buf = NULL;
	mem_chunks_t *dir_buf = (dir) ? &prox->cli_buf : &prox->srv_buf;
	int err = 0;

	/* 
	 * If we received data without content length or chunk size then we'll
	 * keep passing data until the server shuts down on us.
	 */
	if (from->hdr.cnt_len == 0 && from->hdr.cnt_recvd > 0) {
		from->hdr.cnt_len = http_strtol(
		    &from->hdr.hdr[from->hdr.hdr_len], 
		    &from->hdr.hdr[from->hdr.hdr_len + from->hdr.cnt_recvd] -
		    &from->hdr.hdr[from->hdr.hdr_len], 16);
		//http_print_exact(&from->hdr.hdr[from->hdr.hdr_len], 100);
		if (from->hdr.nolen == 1)
			send_until_close = 1;
	}

	/* send whatever content we received when receiving the hdr */
	if ((err = http_send_srv_hdr(lt, prox->srv, &prox->cli)) == -1)
		goto done;

	/* 
	 * hex digits representing the chunk size are not part
	 * of the chunk size, remove them.
	 */
	recvd = from->hdr.cnt_recvd - \
	    (http_strcasechr(&from->hdr.hdr[from->hdr.hdr_len], LF,
	    from->hdr.cnt_recvd) - &from->hdr.hdr[from->hdr.hdr_len]) - 1;


	if (recvd == from->hdr.cnt_len) {
		goto done;
	}

	len = 1;
	do {

		recvd += ret;

		more = recvd < from->hdr.cnt_len ?-1: recvd - from->hdr.cnt_len;
		/* If we received a single complete chunk in the header we'll
		 * assume it is the only chunk. Not a proper assumption at all
		 */
		if (more > 0 && ret == 0)
			break;

		while (more > 0) {
			/* 
			 * skip trailing spaces and don't count them as more
			 * data.
			 */
			while (more && (ret >= more) ) {
				if (isspace(buf->_buf[ret - more])) {
					more--;
				} else
					break;
			}
			/* 
			 * The new chunk size is not in this packet. we recvd
			 * 0 bytes for the new chunk and we still don't know
			 * it's full size.
			 */
			if (more == 0) {
				recvd = 0;
				from->hdr.cnt_len = 0;
				break;
			}
			/* 
			 * At this point ret - `more` points to the new chunk
			 * size, parse it. but first, did we exceed the buffer.
			 */
			if (ret < more)
				break;
			len = http_strtol(&buf->_buf[ret - more],
			    ret - (ret - more), 16);
			/* 
			 * hex digits representing the chunk size are not part
			 * of the chunk size, remove them.
			 */
			recvd = -1 *(http_strcasechr(&buf->_buf[ret - more], LF,
			    ret - (ret - more)) - &buf->_buf[ret - more]) - 1;
			from->hdr.cnt_len = len;
			if (len == 0)
				break;
			/* how much data have we received for the new chunk? */
			recvd += ret - (ret - more);
			/* 
			 * if the chunk was small enough we could have recvd
			 * it all in the same packet.
			 */
			more = (recvd  < len) ? -1 : recvd - len;
		}

		if (len == 0) {
			break;
		}

		CHUNK_ALLOC(buf);
		if (buf == NULL) {
			err = -1;
			goto done;
		}

		ret = http_recv(lt, from, buf->_buf, TCP_BUF_SIZE);

		if (ret == 0) {
			/* same case as of passthrough_exact */
			free(buf);
			err = (send_until_close == 1);
			goto done;
		}

		if (ret == -1) {
			free(buf);
			err = -1;
			goto done;
		}

		buf->sz = ret;
		/*
		 * enqueue the chunk to be written, if we are running out of
		 * total chunks / transaction, then we'll block until it gets
		 * consumed by the other end. This happens when one end is
		 * consuming at a lower speed than the data that is coming in.
		 */
		CHUNK_ENQUEUE(lt, *dir_buf, buf);
		if (more == 0) {
			recvd = 0;
			from->hdr.cnt_len = 0;
		}

	} while (len > 0 || send_until_close);

	err = 0;

done:
	return err;
}

static int
http_passthrough(lthread_t *lt, http_prox_t *prox, http_sess_t *from,
    http_sess_t *to, int dir)
{
	if (from->hdr.chunked)
		return http_passthrough_chunked(lt, prox, from, to, dir);
	 else
		return http_passthrough_exact(lt, prox, from, to, dir);
}

static void
http_listener(lthread_t *lt, lsn_t *lsn)
{
	socklen_t addrlen = 0;
	struct sockaddr cin = {0};
	int c = 0;
	int lsn_fd = 0;
	int counter = 30000;

	DEFINE_LTHREAD(lt);

	lsn_fd = e_listener("127.0.0.1", lsn->lsn_port);
	if (!lsn_fd)
		return;

	printf("Starting listener on %d\n", lsn->lsn_port);

	while (counter) {

		c = lthread_accept(lt, lsn_fd, &cin, &addrlen);
		if (total >= 2000) {
			lthread_sleep(lt, 10);
		}
		total++;

		http_cli_new(lt, lsn, c);

	}

	close(lsn_fd);
}

static void
http_cli_new(lthread_t *lt, lsn_t *lsn, int c)
{
	http_prox_t *prox = NULL;
	lthread_t *lt_cli_rd = NULL, *lt_cli_wr = NULL;
	int opt = 1;
	int ret = 0;

	if (c <= 0) {
		perror("Cannot accept new connection");
		return;
	}

	if ((prox = calloc(1, sizeof(http_prox_t))) == NULL) {
		prox_log(lt, prox, PROX_ERR, PROX_MEM_FAIL, "prox_pool failed");
		return;
	}

	ret = http_save_c_conn_info(c, &prox->cli.sess.conn);
	if (ret == -1) {
		prox_log(lt, prox, PROX_TRC, PROX_GEN_FAIL,
		    "http_save_c_conn_info failed");
		goto err;
	}

	if (setsockopt(c, SOL_SOCKET, SO_REUSEADDR, &opt,sizeof(int)) == -1) {
		perror("failed to set SOREUSEADDR on socket");
	}

	prox->lsn = lsn;
	LIST_INSERT_HEAD(&lsn->active, prox, chain);

	ret = lthread_create(lsn->sched, &lt_cli_rd, http_lt_cli_rd, prox);
	if (ret != 0) {
		prox_log(lt, prox, PROX_ERR, PROX_LT_FAIL, "http_cli_new");
		goto err;
	}

	ret = lthread_create(lsn->sched, &lt_cli_wr, http_lt_cli_wr, prox);
	if (ret != 0) {
		prox_log(lt, prox, PROX_ERR, PROX_LT_FAIL, "http_cli_new");
		goto err;
	}

	if (http_cli_init(lt_cli_rd, prox) == -1) {
		prox_log(lt, prox, PROX_TRC, PROX_GEN_FAIL,
		    "Failed to initialize cli");
		goto err;
	}

	return;
err:

	if (lt_cli_rd)
		lthread_destroy(lt_cli_rd);
	if (lt_cli_wr)
		lthread_destroy(lt_cli_wr);

	shutdown(c, SHUT_RDWR);
	lthread_close(lt, c);
	if (prox)
		free(prox);
}

static int
lsn_mem_init(lsn_t *lsn)
{
	sched_t *sched = NULL;

	if (sched_create(&sched, 0) != 0) {
		perror("Failed to create a scheduler");
		return 1;
	}

	lsn->sched = sched;
	lsn->birth = rdtsc();
	prox_log_new(&lsn->prox_log, "/tmp/", "prox_err.log");
	prox_log_new(&lsn->http_log, "/tmp/", "prox_access.log");
	LIST_INIT(&lsn->active);

	return 0;
}

static void
lsn_run(lsn_t *lsn)
{
	if (lsn == NULL)
		return;
	lthread_t *lt = NULL;
	lthread_create(lsn->sched, &lt, http_listener, lsn);
	lthread_create(lsn->sched, &lt, bd_lt_listener, (void*)0);
	lthread_join(&lsn->sched);
}

static int
handle_args (int argc, char **argv, lsn_t *lsn)
{
	extern char *optarg;
	int c = 0;
	char *errmsg = NULL;
	int err = 0;

	const char *menu = \
	"usage: express [options]\n"
	"-p <port>  : http listener port (default: 3128)\n"
	"-a <ip>    : http listener IP address (default: 0.0.0.0)\n"
	"-i <eth>   : http listener interface\n"
	"-h         : print this help message and exits\n"
	"-l <level> : set log level to info|error|debug|trace\n";

	lsn->lsn_port = 3128;

	while ((c = getopt(argc, argv, "l:hp:")) != -1) {
		switch(c) {
		case 'p':
			lsn->lsn_port = strtol(optarg, NULL, 10);
			if (lsn->lsn_port < 1) {
				errmsg = "port range is between 1 and 65535";
				err = -1;
				goto done;
			}
			break;
		case 'l':
			if (strcasecmp(optarg, "error") == 0)
				lsn->prox_log_level = PROX_ERR;
			else if (strcasecmp(optarg, "warn") == 0)
				lsn->prox_log_level = PROX_WRN;
			else if (strcasecmp(optarg, "trace") == 0)
				lsn->prox_log_level = PROX_TRC;
			else if (strcasecmp(optarg, "debug") == 0)
				lsn->prox_log_level = PROX_DBG;
			else if (strcasecmp(optarg, "info") == 0)
				lsn->prox_log_level = PROX_INFO;
			else
				lsn->prox_log_level = 0;
			break;
		case 'h':
		default:
			err = -1;
			goto done;
		}
	}

done:
	if (err == -1) {
		printf("%s", menu);
		if (errmsg)
			printf("ERROR: %s\n", errmsg);
		return err;
	}

	return 0;
}

int
main(int argc, char *argv[])
{

	struct rlimit limit;
	struct rtprio prio = {.type = RTP_PRIO_REALTIME, .prio = 0};
	lsn_t lsn = {0};
	/* the process by default exits on SIGPIPE, we don't want that */
	signal(SIGPIPE, SIG_IGN);

	if (handle_args(argc, argv, &lsn) == -1) {
		exit(1);
	}

	limit.rlim_cur = limit.rlim_max = 30000;

	if (rtprio(RTP_SET, getpid(), &prio) == -1) {
		perror("Warning: Failed to set rtprio");
	}

	if (lsn_mem_init(&lsn) != 0) {
		perror("Failed to initialize proxy");
		return 1;
	}

	if (setrlimit(RLIMIT_NOFILE, &limit) == -1) {
		perror("Cannot set maximum file limit");
	}

	limit.rlim_cur = limit.rlim_max = 1024*1024*2000;

	if (setrlimit(RLIMIT_AS, &limit) == -1) {
		perror("Cannot set maximum file limit");
	}

	if (setrlimit(RLIMIT_DATA, &limit) == -1) {
		perror("Cannot set maximum file limit");
	}

	if (setrlimit(RLIMIT_SBSIZE, &limit) == -1) {
		perror("Cannot set maximum file limit");
	}


	lsn_run(&lsn);
	printf("Exiting...!\n");

	return 0;
}
