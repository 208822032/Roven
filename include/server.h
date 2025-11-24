



#ifndef __SERVER_H__
#define __SERVER_H__

#include "resp_protocol.h"

#define BUFFER_LENGTH		4096

#define ENABLE_HTTP			0
#define ENABLE_WEBSOCKET	0
#define ENABLE_KVSTORE		1


typedef int (*RCALLBACK)(int fd);


struct conn {
	int fd;

#if 0
	char rbuffer[BUFFER_LENGTH];
	int rlength;
#else
	//动态接收缓冲区
	char *rbuffer;
	size_t rbuffer_size;
	size_t rlength;
#endif

#if 0
	char wbuffer[BUFFER_LENGTH];
	int wlength;
#else
	char *wbuffer;
	size_t wbuffer_size;
	size_t wlength;
#endif

	RCALLBACK send_callback;

	union {
		RCALLBACK recv_callback;
		RCALLBACK accept_callback;
	} r_action;

	resp_parser_t parser;
	int parser_initialized;

	int status;
#if 1 // websocket
	char *payload;
	char mask[4];
#endif

	//是否做过首次对齐检查
	int first_packet_checked;
};

#if ENABLE_HTTP
int http_request(struct conn *c);
int http_response(struct conn *c);
#endif

#if ENABLE_WEBSOCKET
int ws_request(struct conn *c);
int ws_response(struct conn *c);
#endif

#if ENABLE_KVSTORE
int kvs_request(struct conn *c);
int kvs_response(struct conn *c);

#endif




#endif


