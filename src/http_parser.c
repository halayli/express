#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "http.h"

int
http_parse_req_line(http_cli_t *c)
{
	typedef enum {st_host = 0,
	    st_uri,
	    st_proto,
	    st_port} st_state;
	int uri_start = 0;
	int port_start = 0;
	int host_start = 0;
	int proto_start	= 0;
	char *hdr = c->sess.hdr.hdr;
	int len = c->sess.hdr.hdr_len;
	st_state state = st_host;
	int p = 0;
	int strip = 0;
	c->req.port = HTTP_DEFAULT_SERVER_PORT;

	if (len < 7)
		return HTTP_ERR_INV_REQ_LINE;

	if (strncmp(hdr, "GET", 3) == 0) {
		c->req.method = HTTP_GET;
		c->req.method_str = "GET";
		p += 3;
	} else if (strncmp(hdr, "PUT", 3) == 0) {
		c->req.method = HTTP_PUT;
		c->req.method_str = "PUT";
		p += 3;
	} else if (strncmp(hdr, "DELETE", 6) == 0) {
		c->req.method = HTTP_DELETE;
		c->req.method_str = "DELETE";
		p += 6;
	} else if (strncmp(hdr, "POST", 4) == 0) {
		c->req.method = HTTP_POST;
		c->req.method_str = "POST";
		p += 4;
	} else if (strncmp(hdr, "HEAD", 4) == 0) {
		c->req.method = HTTP_HEAD;
		c->req.method_str = "HEAD";
		p += 4;
	} else  {
		return HTTP_ERR_INV_METHOD;
	}

	while (p < len && HTTP_IS_SEP(hdr[p]))
		p++;
	if (p == len)
		return HTTP_ERR_INV_REQ_LINE;

	if (((p + 7) < len) && strncmp(&hdr[p], "http://", 7) == 0) {
		p += 7;
		strip = p;
		state = st_host;
		host_start = p;
	} else {
		state = st_uri;
	}

	while (p < len) {
		switch (state) {
		case st_host:
			if (HTTP_IS_HOST_TERMINATOR((unsigned char)hdr[p])) { 
				if (host_start == p)
					return HTTP_ERR_INV_HOST;
				if ((p - host_start) > HTTP_MAX_HOST_LEN)
					return -1;
				if ((c->req.host = strndup(&hdr[host_start], p - host_start)) == NULL)
					return -1;
				if (hdr[p] == ':') {
					state = st_port;
					p++;
					goto st_port;
				} else {
					state = st_uri;
					goto st_uri;
				}
			}

			if (!HTTP_IS_HOST_TOKEN(hdr[p]))
				return HTTP_ERR_INV_HOST;
			p++;
			break;
		case st_port:
		st_port:
			port_start = p;
			while (p < len && HTTP_IS_DIGIT(hdr[p]))
				p++;

			if (port_start == p)
				return HTTP_ERR_INV_PORT;
			c->req.port = http_strtol(&hdr[port_start],
			    p - port_start, 10);
			if (c->req.port > 65534 || c->req.port < 1)
				return HTTP_ERR_INV_PORT;
			state = st_uri;
			goto st_uri;
		case st_uri:
		st_uri:
			uri_start = p;

			while (p < len && !HTTP_IS_SEP(hdr[p]))
				p++;

			if (p == len)
				return HTTP_ERR_INV_REQ_LINE;
			if ((c->req.uri = strndup(&hdr[uri_start], p - uri_start)) == NULL)
				abort();
			while (p < len && isspace(hdr[p])) {
				p++;
			}
			state = st_proto;
			goto st_proto;
		case st_proto:
		st_proto:
			proto_start = p;

			while (p < len && !HTTP_IS_CR_OR_LF(hdr[p]))
			p++;

			if (p == len){
				return HTTP_ERR_INV_REQ_LINE;
			}

			if ((p - proto_start) != 8)
				return HTTP_ERR_INV_PROTO;

			if (strncmp(&hdr[proto_start], "HTTP/1.1", 8) == 0) {
				c->sess.hdr.http11 = 1;
			} else if (
			    strncmp(&hdr[proto_start], "HTTP/1.0", 8) == 0) {
				c->sess.hdr.http11 = 0;
			} else {
				return HTTP_ERR_INV_PROTO;
			}

			while (p < len && hdr[p] == '\0')
				p++;

			c->sess.hdr.hdr[p - 1] = '\0';
			c->sess.hdr.hdr_start = p;

			return 0;
		}
	}

	return HTTP_ERR_INV_REQ_LINE;
}

int inline
http_parse_resp_line(http_srv_t *s)
{
	typedef enum {st_code = 0, st_word} st_state;
	int code_start, word_start;
	char *hdr = s->sess.hdr.hdr;
	int len = s->sess.hdr.hdr_len;
	st_state state = st_code;
	int p = 0;

	if (len < 13)
		return HTTP_ERR_INV_RESP_LINE;

	if (http_strcmp8(&hdr[p], "HTTP/1.1")) {
		s->sess.hdr.http11 = 1;
	}
	else if (http_strcmp8(&hdr[p], "HTTP/1.0")) {
		s->sess.hdr.http11 = 0;
	} else {
		return HTTP_ERR_INV_RESP_LINE;
	}

	p += 8;
	while (p < len && HTTP_IS_SEP(hdr[p]))
		p++;

	if (p == len)
		return HTTP_ERR_INV_RESP_LINE;

	state = st_code;
	while (p < len) {
		switch(state) {
		case st_code:
			code_start = p;
			while (p < len && HTTP_IS_DIGIT(hdr[p]))
				p++;
			if (code_start == p)
				return HTTP_ERR_INV_RESP_CODE;
			s->resp.resp_code = http_strtol(&hdr[code_start],
			    p - code_start, 10);
			word_start = p;
			s->resp.resp_code_str = word_start;
			state = st_word;
			goto st_word;
		case st_word:
		st_word:
			while (p < len && hdr[p++] != LF);
			s->sess.hdr.hdr_start = p;
			s->sess.hdr.hdr[p - 1] = '\0';
			return 0;
		}
	}

	return HTTP_ERR_INV_RESP_LINE;
}

int
http_parse_req_hdr(http_cli_t *c)
{
	int ret = 0;
	char *tmp = NULL, *p = NULL;

	if ((ret = http_parse_req_line(c)) != 0)
		return ret;
	if ((ret = http_parse_hdr(&c->sess)) != 0)
		return ret;

	tmp = (char *)h_get(c->sess.hdr.hdrs, "content-length");
	if (tmp)
		c->sess.hdr.cnt_len = strtol(tmp, NULL, 10);
	tmp = (char *)h_get(c->sess.hdr.hdrs, "transfer-encoding");
	if (tmp)
		c->sess.hdr.chunked = 1;

	
	tmp = (char *)h_get(c->sess.hdr.hdrs, "host");
	if (tmp) {
		if ((p = strchr(tmp, ':')) != NULL) {
			if ((p - tmp) > HTTP_MAX_HOST_LEN)
				return -1;
			if (c->req.host)
				free(c->req.host);
			if ((c->req.host = strndup(tmp, p - tmp)) == NULL)
				return -1;
			c->req.port = strtol(p, NULL, 10);
		} else {
			if (c->req.host)
				free(c->req.host);
			if ((c->req.host = strndup(tmp, p - tmp)) == NULL)
				return -1;
		}
	}



	return 0;
}

int
http_parse_resp_hdr(http_srv_t *s)
{
	int ret = 0;
	char *tmp = NULL;

	if ((ret = http_parse_resp_line(s)) != 0)
		return ret;
	if ((ret = http_parse_hdr(&s->sess)) != 0)
		return ret;

	tmp = (char *)h_get(s->sess.hdr.hdrs, "content-length");
	if (tmp)
		s->sess.hdr.cnt_len = strtol(tmp, NULL, 10);
	else
		s->sess.hdr.nolen = 1;
	tmp = (char *)h_get(s->sess.hdr.hdrs, "transfer-encoding");
	if (tmp)
		s->sess.hdr.chunked = 1;

	return 0;
}

int
http_parse_hdr(http_sess_t *sess)
{
	int i = 0;
	char *p = NULL, *q = NULL;
	int start = sess->hdr.hdr_start;

	for (i = sess->hdr.hdr_start; i < sess->hdr.hdr_len; i++) {
		if (sess->hdr.hdr[i] == '\n') {
			/* check if hdr is continuing on new line */
			if ((sess->hdr.hdr_len - i) &&
		    		!HTTP_IS_SEP(sess->hdr.hdr[i + 1]))
				sess->hdr.hdr[i] = '\0';
			else
				continue;

			/* terminate at \r if it exist */
			if (i && sess->hdr.hdr[i - 1] == '\r')
				sess->hdr.hdr[i - 1] = '\0';

			p = strchr(&sess->hdr.hdr[start], ':');
			if (p == NULL) {
				start = i + 1;
				continue;
			}

			*p = '\0';
			for (q = &sess->hdr.hdr[start]; *q; q++)
				*q = tolower(*q);
			p++;
			while (*p && !HTTP_IS_SEP(*p))
				p++;
			h_insert(sess->hdr.hdrs, &sess->hdr.hdr[start], p + 1);
			start = i + 1;
		}
	}

	return 0;
}
