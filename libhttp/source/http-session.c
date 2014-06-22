#include "cstringext.h"
#include "sys/sock.h"
#include "http-parser.h"
#include <stdlib.h>
#include <memory.h>
#include <assert.h>
#include "http-server-internal.h"

static int http_session_start(struct http_session_t *session);

// create a new http session
static struct http_session_t* http_session_new()
{
	struct http_session_t* session;

	session = (struct http_session_t*)malloc(sizeof(session[0]));
	if(!session)
		return NULL;

	memset(session, 0, sizeof(session[0]));
	session->parser = http_parser_create(HTTP_PARSER_SERVER);
	return session;
}

static void http_session_free(struct http_session_t *session)
{
	if(session->socket)
		aio_socket_destroy(session->socket);

	if(session->parser)
		http_parser_destroy(session->parser);

	free(session);
}

// reuse/create a http session
static struct http_session_t* http_session_alloc()
{
	struct http_session_t* session;

	// TODO: reuse ? 
	session = http_session_new();
	if(!session)
		return NULL;

	return session;
}

static void http_session_handle(struct http_session_t *session)
{
	const char* uri = http_get_request_uri(session->parser);
	const char* method = http_get_request_method(session->parser);

	if(session->server->handle)
	{
		session->server->handle(session->server->param, session, method, uri);
	}
}

static void http_session_onrecv(void* param, int code, int bytes)
{
	struct http_session_t *session;
	session = (struct http_session_t*)param;

	// peer close socket, don't receive all data
	//if(0 == code && 0 == bytes) 

	if(0 == code && bytes > 0)
	{
		int remain = bytes;
		code = http_parser_input(session->parser, session->data, &remain);
		if(0 == code)
		{
			session->data[0] = '\0'; // clear for save user-defined header

			// call
			http_session_handle(session);

			// restart
			http_session_start(session);
		}
		else if(1 == code)
		{
			code = aio_socket_recv(session->socket, session->data, sizeof(session->data), http_session_onrecv, session);
		}
	}

	if(code < 0 || 0 == bytes)
	{
		free(session);
		printf("http_session_onrecv => %d\n", 0==bytes ? 0 : code);
	}
}

static void http_session_onsend(void* param, int code, int bytes)
{
	struct http_session_t *session;
	session = (struct http_session_t*)param;

	if(code < 0)
	{
		http_session_free(session);
	}
}

static int http_session_start(struct http_session_t *session)
{
	int r;

	// clear parser status
	http_parser_clear(session->parser);

	// receive client request
	r = aio_socket_recv(session->socket, session->data, sizeof(session->data), http_session_onrecv, session);
	if(0 != r)
	{
		printf("http_session_run recv => %d\n", r);
		http_session_free(session);
		return -1;
	}

	return 0;
}

struct http_session_t* http_session_run(struct http_server_t *server, socket_t socket, const char* ip, int port)
{
	int r;
	struct http_session_t *session;

	session = http_session_alloc();
	if(!session) return NULL;

	session->server = server;
	session->socket = aio_socket_create(socket, 1);

	r = http_session_start(session);

	return 0==r ? session : NULL;
}

// Request
int http_server_get_host(void* param, void** ip, int *port)
{
	struct http_session_t *session;
	session = (struct http_session_t*)param;

	return 0;
}

const char* http_server_get_header(void* param, const char *name)
{
	struct http_session_t *session;
	session = (struct http_session_t*)param;

	return http_get_header_by_name(session->parser, name);
}

int http_server_get_content(void* param, void **content, int *length)
{
	struct http_session_t *session;
	session = (struct http_session_t*)param;

	*content = (void*)http_get_content(session->parser);
	*length = http_get_content_length(session->parser);
	return 0;
}

int http_server_send(void* param, int code, const void* data, int bytes)
{
	int r;
	char msg[1024];
	socket_bufvec_t vec[2];
	struct http_session_t *session;
	session = (struct http_session_t*)param;

	snprintf(msg, sizeof(msg), 
		"HTTP/1.1 %d OK\r\n"
		"Server: WebServer 0.2\r\n"
		"Connection: keep-alive\r\n"
		"Keep-Alive: timeout=5,max=100\r\n"
		"%s" // user-defined headers
		"Content-Length: %d\r\n\r\n", 
		code, session->data, bytes);

	assert(strlen(msg) < 1000);
	socket_setbufvec(vec, 0, msg, strlen(msg));
	socket_setbufvec(vec, 1, (void*)data, bytes);

	r = aio_socket_send_v(session->socket, vec, 2, http_session_onsend, session);
	if(0 != r)
	{
		http_session_free(session);
	}

	return r;
}

int http_server_set_header(void* param, const char* name, const char* value)
{
	char msg[512];
	struct http_session_t *session;
	session = (struct http_session_t*)param;

	snprintf(msg, sizeof(msg), "%s: %s\r\n", name, value);
	strcat(session->data, msg);
	return 0;
}

int http_server_set_header_int(void* param, const char* name, int value)
{
	char msg[512];
	struct http_session_t *session;
	session = (struct http_session_t*)param;

	snprintf(msg, sizeof(msg), "%s: %d\r\n", name, value);
	strcat(session->data, msg);
	return 0;
}

int http_server_set_content_type(void* session, const char* value)
{
	//Content-Type: application/json
	//Content-Type: text/html; charset=utf-8
	return http_server_set_header(session, "Content-Type", value);
}