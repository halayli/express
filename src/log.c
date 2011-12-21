#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lthread.h>

#include "http.h"
#include "log.h"

#define PROX_LOG_FMT "%s %llu lt(%d) "

char *prox_levels[] = {
    [PROX_INFO] = "PROX INFO: ",
    [PROX_WRN] = "PROX WARN: ",
    [PROX_TRC] = "PROX TRC: ",
    [PROX_DBG] = "PROX DBG: ",
    [PROX_ERR] = "PROX ERR: ",
};

char *err_map[] = {
	[PROX_PASSTHRU] = \
	    PROX_LOG_FMT "pass through: %d\n",

	[PROX_SRV_CONN] = \
	     PROX_LOG_FMT "srv_rd server connected: %s\n",
	[PROX_SRV_DISC] = \
	     PROX_LOG_FMT "srv_rd server disconnected: %s\n",
	[PROX_SRV_WR_START] = \
	     PROX_LOG_FMT "srv_wr started\n",
	[PROX_SRV_WR_WAIT] = \
	     PROX_LOG_FMT "srv_wr waiting for chunk\n",
	[PROX_SRV_WR_WAIT_DONE] = \
	     PROX_LOG_FMT "srv_wr waiting for chunk done\n",
	[PROX_SRV_WR_DONE] = \
	     PROX_LOG_FMT "srv_wr is done\n",

	[PROX_CLI_WR_WAIT] = \
	     PROX_LOG_FMT "cli_wr waiting for chunk\n",
	[PROX_CLI_WR_START] = \
	     PROX_LOG_FMT "cli_wr started\n",
	[PROX_CLI_WR_WAIT_DONE] = \
	     PROX_LOG_FMT "cli_wr waiting for chunk done\n",
	[PROX_CLI_WR_DONE] = \
	     PROX_LOG_FMT "cli_wr is done\n",
	[PROX_CLI_CONN] = \
	     PROX_LOG_FMT "cli_rd client %s:%d connected\n",
	[PROX_CLI_DISC] = \
	     PROX_LOG_FMT "cli_rd client %s:%d disconnected\n",
	[PROX_CONN_FAIL] = \
	     PROX_LOG_FMT "client connection failed\n",
	[PROX_GEN_FAIL] = \
	     PROX_LOG_FMT "prox trans error: %s\n",

	[PROX_SRV_EARLY_RESP] = \
	     PROX_LOG_FMT "received early response from server\n",
	[PROX_CLI_EARLY_REQ] = \
	     PROX_LOG_FMT "received early req from client\n",

	[PROX_MEM_FAIL] = \
	     PROX_LOG_FMT "failed to alloced memory: %s\n",
	[PROX_LT_FAIL] = \
	     PROX_LOG_FMT "failed to create lt in %s\n",
	[PROX_GEN_FAIL] = \
	     PROX_LOG_FMT "prox error: %s\n",
	[PROX_HTTP_GEN_FAIL] = \
	     PROX_LOG_FMT "prox http error: %s\n",

	[PROX_CLI_REQ] = \
	     PROX_LOG_FMT "cli req\n",
	[PROX_SRV_RESP] = \
	     PROX_LOG_FMT "srv resp\n",
};

int
prox_log_new(prox_log_t *log, char *path, char *filename)
{
	int fd;

	if ((strlen(path) + strlen(filename)) > 255)
		goto err;

	strcpy(log->fullpath, path);
	strcat(log->fullpath, filename);

	fd = open(log->fullpath, O_RDWR | O_NONBLOCK | O_APPEND | O_CREAT);
	fd = 0;

	if ((log->f = fdopen(fd, "w")) == NULL)
		goto err;

	return 0;

err:
	perror("Failed to initialize log");

	return -1;
}
