#ifndef _PROX_LOG_H_
#define _PROX_LOG_H_
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include <lthread.h>

#include "common/rdtsc.h"
#include "http.h"

/*
 * logger functionalities
 */
typedef struct {
	char fullpath[255];
	int fd;
	FILE *f;
} prox_log_t;

enum {
	PROX_INFO,
	PROX_WRN,
	PROX_TRC,
	PROX_DBG,
	PROX_ERR,
};

extern char *prox_levels[];

typedef enum {
	PROX_SRV_EARLY_RESP,
	PROX_CLI_EARLY_REQ,
	PROX_CLI_POST_FAIL,

	PROX_LT_FAIL,
	PROX_GEN_FAIL,
	PROX_HTTP_GEN_FAIL,
	PROX_MEM_FAIL,

	PROX_PASSTHRU,
	PROX_CONN_FAIL,

	PROX_SRV_CONN,
	PROX_SRV_DISC,
	PROX_SRV_WR_WAIT,
	PROX_SRV_WR_WAIT_DONE,
	PROX_SRV_WR_START,
	PROX_SRV_WR_DONE,

	PROX_CLI_CONN,
	PROX_CLI_DISC,
	PROX_CLI_WR_WAIT,
	PROX_CLI_WR_WAIT_DONE,
	PROX_CLI_WR_START,
	PROX_CLI_WR_DONE,

	PROX_CLI_REQ,
	PROX_SRV_RESP,
} prox_err_t;

extern char *err_map[];
int prox_log_new(prox_log_t *l, char *path, char *filename);

#define LOG_PRINT(_level, _log, _fmt, _args...) do {   			\
	if (((_level <= (prox)->lsn->prox_log_level)) ||		\
	    ((PROX_ERR == (prox)->lsn->prox_log_level))){ 		\
		fprintf(prox->lsn->prox_log.f, _fmt,			\
		    prox_levels[_level],				\
		    tick_diff_secs(prox->lsn->birth, rdtsc()), ##_args);\
		fflush(prox->lsn->prox_log.f);				\
	}								\
	if (prox->lsn->prox_log_level == PROX_DBG) {			\
		if (_log == PROX_CLI_REQ)				\
			http_print_exact(prox->cli.sess.hdr.hdr,	\
		    	prox->cli.sess.hdr.hdr_len);			\
		if (_log == PROX_SRV_RESP)				\
			http_print_exact(prox->srv->sess.hdr.hdr,	\
		    	prox->srv->sess.hdr.hdr_len);			\
	}								\
    } while(0)								\

#define prox_log(lt, prox, _level, _log, args...)           		\
    LOG_PRINT(_level, _log, err_map[_log], lthread_id(lt), ##args)     	\

#endif
