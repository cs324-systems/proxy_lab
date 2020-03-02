#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define __MAC_OS_X

void *thread(void *vargp);
void doit(int fd);
void store_requesthdrs(rio_t * rp, char * headers);
void create_msg(char *path, char* headers, char *hostname, char *msg);
int send_recv_message(char *hostname, char *port, char *request,
	 int requestlen, char *response);
int check_if_proxy(char *uri);
void parse_uri_proxy(char *uri, char *hostname, char *port, char *path);
void clienterror(int fd, char *cause, char *errnum,
	    char *shortmsg, char *longmsg);
void echo(int connfd);

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";


int main(int argc, char **argv) {
	int		listenfd  , *connfd;
	socklen_t	clientlen;
	struct sockaddr_storage clientaddr;
  pthread_t tid;

	/* Check command line args */
	if (argc != 2) {
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(1);
	}
	listenfd = Open_listenfd(argv[1]);
	while (1) {
    clientlen = sizeof(struct sockaddr_storage);
    connfd = (int *) Malloc(sizeof(int));
    *connfd = Accept(listenfd, (SA*) &clientaddr, &clientlen);
    Pthread_create(&tid, NULL, thread, connfd);
  }
}

void *thread(void *vargp) {
  int connfd = *((int *) vargp);
  Pthread_detach(pthread_self());
  Free(vargp);
  doit(connfd);
  Close(connfd);
  return NULL;
}
/*
 * doit - handle one HTTP request/response transaction
 */
/* $begin doit */
void doit(int fd) {
	int is_proxy;
	char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
	char headers[MAXLINE];
	char hostname[MAXLINE], port[MAXLINE], path[MAXLINE];
	char msg[MAXLINE];
	char response[MAX_OBJECT_SIZE];
	rio_t		rio;

	/* Read request line and headers */
	Rio_readinitb(&rio, fd);
	if (!Rio_readlineb(&rio, buf, MAXLINE)) {
		return;
  }
	printf("%s", buf);
	sscanf(buf, "%s %s %s", method, uri, version);
	if (strcasecmp(method, "GET") != 0) { //if it doesn't equal GET
		clienterror(fd, method, "501", "Not Implemented",
				    "Proxy does not implement this method");
		return;
  }
	store_requesthdrs(&rio, headers);

	is_proxy = check_if_proxy(uri);

	if (is_proxy) {
		parse_uri_proxy(uri, hostname, port, path);
		create_msg(path, headers, hostname, msg);
		//printf("hostname: %s\n", hostname);
		//printf("port: %s\n", port);
		//printf("path: %s\n", path);
		//printf("Request:\n");
		//printf("%s", msg);
		int response_size = send_recv_message(hostname, port, msg, strlen(msg), response);
		//printf("Response:\n");
		//printf("%s", response);
		//printf("Response length = %d\n", response_size);
		Rio_writen(fd, response, response_size);

	}
	else {
		char *error_msg = "Not a valid proxy request";
		Rio_writen(fd, error_msg, strlen(error_msg));
	}
}
/* $end doit */

void store_requesthdrs(rio_t * rp, char * headers) {
	char		buf       [MAXLINE];

	Rio_readlineb(rp, buf, MAXLINE);
	printf("%s", buf);
	strcat(headers, buf);
	while (strcmp(buf, "\r\n")) {
		Rio_readlineb(rp, buf, MAXLINE);
		printf("%s", buf);
		strcat(headers, buf);
	}
	return;
}

void create_msg(char *path, char* headers, char *hostname, char *msg) {
	strcat(msg, "GET ");
	strcat(msg, path);
	strcat(msg, " HTTP/1.0\r\n");
	if (headers != NULL && headers[0] != '\0') {
		strcat(msg, headers); //add the headers that were already there
	}
	char *hostIndex = strstr(headers, "Host"); //if no host, add host header
	if (hostIndex == NULL) {
		strcat(msg, "Host: ");
		strcat(msg, hostname);
		strcat(msg, "\r\n");
	}
	if (strstr(headers, "User-Agent") == NULL) {
		strcat(msg, user_agent_hdr);
	}
  strcat(msg, connection_hdr);
	strcat(msg, proxy_connection_hdr);
	strcat(msg, "\r\n");
}

int send_recv_message(char *hostname, char *port, char *request, int requestlen, char *response) {
	int skt = Open_clientfd(hostname, port);
  send(skt, request, requestlen, 0);
  int responseSize = recv(skt, response, MAXLINE - 1, 0);
	int numBytes = 0;

	char *contentLengthPos = strstr(response, "Content-length:");
	char length[20];
	strncpy(length, contentLengthPos + 16, 18);
	int contentLength = atoi(length);

	while(numBytes < contentLength) {
		int tempSize = recv(skt, response + responseSize, contentLength - numBytes, 0);
		responseSize += tempSize;
		numBytes += tempSize;
	}

  return responseSize;
}

/*
 * Returns 1 if it is a proxy, 0 if not
*/
int check_if_proxy(char *uri) {
	if (uri[0] != '/') { //&& strstr(uri, "http://")) {
		return 1;
	}
	return 0;
}

void parse_uri_proxy(char *uri, char *hostname, char *port, char *path) {
	char tmpURI[MAXLINE];
	strcpy(tmpURI, uri + 6);

	if (strstr(tmpURI, ":")) { //There's a port
		sscanf(uri, "http://%99[^:]:%10[^/]%199[^\n]", hostname, port, path);
	}
	else { //there's no port
		port[0] = '8';
		port[1] = '0';
		sscanf(uri, "http://%99[^/]%199[^\n]", hostname, path);
	}
}

/*
 * clienterror - returns an error message to the client
 */
/* $begin clienterror */
void clienterror(int fd, char *cause, char *errnum,
	    char *shortmsg, char *longmsg) {
	char		buf       [MAXLINE], body[MAXBUF];

	/* Build the HTTP response body */
	sprintf(body, "<html><title>Tiny Error</title>");
	sprintf(body, "%s<body bgcolor=" "ffffff" ">\r\n", body);
	sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
	sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
	sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

	/* Print the HTTP response */
	sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
	Rio_writen(fd, buf, strlen(buf));
	sprintf(buf, "Content-type: text/html\r\n");
	Rio_writen(fd, buf, strlen(buf));
	sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
	Rio_writen(fd, buf, strlen(buf));
	Rio_writen(fd, body, strlen(body));
}
/* $end clienterror */

void echo(int connfd) {
  int n;
  char buf[MAXLINE];
  rio_t rio;

  Rio_readinitb(&rio, connfd);
  while((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
		printf("server received %d bytes\n", n);
		Rio_writen(connfd, buf, n);
  }
}
