#include <unistd.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <err.h>

#include "common/mem.h"
#include "common/time.h"
#include "http.h"
#include "log.h"

extern uint64_t rel_time;

static int http_save_conn_info(int fd, http_conn_t *conn, int);
static int http_hash_domain_port(http_prox_t *prox);

http_srv_t*
http_select_srv(http_prox_t *prox)
{
	int port = prox->cli.req.port;
	http_srv_t *s;
	int entry;
	entry = http_hash_domain_port(prox);

	LIST_FOREACH(s, &prox->srvs[entry], chain) {
		if (strcasecmp(s->sess.conn.srv_host,
		    prox->cli.req.host) == 0 &&
		    htons(s->sess.conn.srv_port) == port) {
			return s;
		}
	}

	return NULL;
}

int
http_hash_domain_port(http_prox_t *prox)
{
	char tmp[260];
	int port = prox->cli.req.port;
	char *host = prox->cli.req.host;
	int len = 0;

	len = sprintf(tmp, "%s:%d", host, port);
	return h_hash_func(tmp, len) % 20;
}

int
http_connect(lthread_t *lt, http_prox_t *prox)
{

	int fd = 0;
	int ret = 0;
	int retries = 5;
	int sleep = 50;
	int entry = 0;
	struct hostent *host_ip = NULL;
	struct sockaddr_in name;
	http_srv_t *srv = NULL;
	char *host = prox->cli.req.host;
	int success = 0;
	
	entry = http_hash_domain_port(prox);

	name.sin_family = AF_INET;
	name.sin_port = htons(prox->cli.req.port);

	//printf("host %s port : %d\n", host, prox->cli.req.port);
	//snprintf(tmp, strlen(prox->cli.req.host) + 1, "%s", host);

	if (!inet_pton(AF_INET, host, &name.sin_addr)) {
		if ((host_ip = gethostbyname(host)) == NULL)
			return -1;
		bcopy(host_ip->h_addr, &name.sin_addr, host_ip->h_length);
	}

	while (retries--){
		fd = lthread_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		//printf("%ld connecting with fd %d\n", lthread_id(lt), fd);
		if (fd == -1) {
			perror("Error creating new socket");
			continue;
		}

		ret = lthread_connect(lt, fd, (struct sockaddr *)&name,
		    sizeof(struct sockaddr_in), 5000);

		if (ret < 0) {
			perror("Error connecting to host");
			lthread_close(lt, fd);
			lthread_sleep(lt, sleep);
			sleep += 10;
			continue;
		}

		if ((srv = calloc(1, sizeof(http_srv_t))) == NULL) {
			lthread_close(lt, fd);
			perror("Failed to allocate server structure");
			break;
		}
		if ((srv->sess.hdr.hdrs = h_init(128)) == NULL) {
			free(srv);
			lthread_close(lt, fd);
			perror("Failed to allocate server structure");
			break;
		}

		if (http_save_s_conn_info(fd, &srv->sess.conn) != 0) {
			free(srv->sess.hdr.hdrs);
			free(srv);
			lthread_close(lt, fd);
			lthread_sleep(lt, sleep);
			sleep += 10;
			continue;
	    	}

		LIST_INSERT_HEAD(&prox->srvs[entry], srv, chain);
		success = 1;
		break;
	}

	if (!success)
		return -1;

	/* XXX: host might be > 255 */
	srv->sess.conn.shutdown_rd = 0;
	srv->sess.conn.shutdown_wr = 0;
	srv->ref = 0;
	prox->srv = srv;

	if ((srv->sess.conn.srv_host = strdup(prox->cli.req.host)) == NULL)
		abort();

	srv->st = ST_TRANS_INIT;

	return 0;
}

int
http_save_c_conn_info(int fd, http_conn_t *conn)
{
	return http_save_conn_info(fd, conn, 0);
}

int
http_save_s_conn_info(int fd, http_conn_t *conn)
{
	return http_save_conn_info(fd, conn, 1);
}

static int
http_save_conn_info(int fd, http_conn_t *conn, int dir)
{
	struct sockaddr_in peer = {0};
	struct sockaddr_in srv = {0};
	struct sockaddr_in tmp = {0};
	socklen_t len = sizeof(struct sockaddr_in);

	if (getpeername(fd, (struct sockaddr *)&peer, &len) == -1) {
		//perror("Cannot get peername");
		return -1;
	}

	if (getsockname(fd, (struct sockaddr *)&srv, &len) == -1) {
		//perror("Cannot get sockname");
		return -1;
	}

	/* 
	 * The peer whom we are connected to (server) or who's connected to us.
	 * This is based on the client or server perspective.
	 */
	if (dir) {
		tmp = peer;
		peer = srv;
		srv = tmp;
	}

	conn->fd = fd;
	/* peer's ip */
	conn->peer_addr = peer.sin_addr;
	/* peer's port we connected to */
	conn->peer_port = peer.sin_port;
	/* (proxy)'s ephemeral port */
	conn->srv_port = srv.sin_port;
	/* (proxy)'s ip */
	conn->srv_addr = srv.sin_addr;

	/*
	printf("client addr is: %s\n", inet_ntoa(conn->peer_addr));
	printf("server addr is: %s\n", inet_ntoa(conn->srv_addr));

	printf("client port is: %d\n", htons(conn->peer_port));
	printf("server port is: %d\n", htons(conn->srv_port));
	*/
	return 0;
}

int
http_send(lthread_t *lt, http_sess_t *c, char *buf, uint32_t len)
{
	//http_print_exact(buf, len);
	return lthread_send(lt, c->conn.fd, buf, len, 0);
}

int
http_recv(lthread_t *lt, http_sess_t *c, char *buf, uint32_t len)
{
	return lthread_recv(lt, c->conn.fd, buf, len, 0, 10000);
}
