#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define __MAC_OS_X
#define NTHREADS 5
#define SBUFSIZE 16
#define LOG_MSG_LEN 50

typedef struct {
	int *buf;
	int n;
	int front;
	int rear;
	sem_t mutex;
	sem_t slots;
	sem_t items;
} sbuf_t;

typedef struct {
  char **buf;
  int n;
  int front;
  int rear;
  sem_t mutex;
  sem_t slots;
  sem_t items;
} logbuf_t;

typedef struct cache_line cache_line;

struct cache_line {
  char *data;
  int size;
  char *uri;
  cache_line *next;
};

typedef struct {
	cache_line *head;
	int readcnt;
	int writecnt;
	sem_t outerQ;
	sem_t rsem;
	sem_t rmutex;
	sem_t wmutex;
	sem_t wsem;
} cache_buf;

void sbuf_init(sbuf_t *sp, int n);
void sbuf_deinit(sbuf_t *sp);
void sbuf_insert(sbuf_t *sp, int item);
int sbuf_remove(sbuf_t *sp);

void logbuf_init(logbuf_t *lp, int n);
void log_buf_deinit(logbuf_t *lp);
void logbuf_insert(logbuf_t *lp, char *item);
char *logbuf_remove(logbuf_t *lp);

void cache_buf_init(cache_buf *cache);

//void sigint_handler(int sig);

void *log_thread(void *vargp);
void *thread(void *vargp);
void doit(int fd);
void store_requesthdrs(rio_t * rp, char * headers);
void create_msg(char *path, char* headers, char *hostname, char *msg);
int send_recv_message(char *hostname, char *port, char *request,
	 int requestlen, char *response);
int check_if_proxy(char *uri);
void parse_uri_proxy(char *uri, char *hostname, char *port, char *path);

int read_cache(cache_buf *cache, char *uri);
void write_cache(cache_buf *cache, cache_line **response, char *uri);
cache_line* check_cache(char *uri);
void cache_insert(cache_line *response);
void delete_last();
void free_cache(cache_buf *cache);

void clienterror(int fd, char *cause, char *errnum,
	    char *shortmsg, char *longmsg);
void echo(int connfd);

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";

sbuf_t sbuf;
logbuf_t logbuf;
cache_buf cache;
FILE *file;
int cache_size;

int main(int argc, char **argv) {
	int i, listenfd;//, *connfd;
	int connfd;
	socklen_t	clientlen;
	struct sockaddr_storage clientaddr;
  pthread_t tid;
  pthread_t logtid;
  cache_size = 0;

	/* Check command line args */
	if (argc != 2) {
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(1);
	}
	listenfd = Open_listenfd(argv[1]);
	sbuf_init(&sbuf, SBUFSIZE);
	logbuf_init(&logbuf, SBUFSIZE);
	cache_buf_init(&cache);

	//Signal(SIGINT,  sigint_handler);   /* ctrl-c */

	Pthread_create(&logtid, NULL, log_thread, NULL);

	for (i = 0; i < NTHREADS; i++) {
		Pthread_create(&tid, NULL, thread, NULL);
	}

	while (1) {
    clientlen = sizeof(struct sockaddr_storage);
		connfd = Accept(listenfd, (SA*) &clientaddr, &clientlen);
		sbuf_insert(&sbuf, connfd);
    //connfd = (int *) Malloc(sizeof(int));
    //*connfd = Accept(listenfd, (SA*) &clientaddr, &clientlen);
    //Pthread_create(&tid, NULL, thread, connfd);
  }
}

void sbuf_init(sbuf_t *sp, int n) {
	sp->buf = Calloc(n, sizeof(int));
	sp->n = n;
	sp->front = sp->rear = 0;
	Sem_init(&sp->mutex, 0, 1);
	Sem_init(&sp->slots, 0, n);
	Sem_init(&sp->items, 0, 0);
}

void sbuf_deinit(sbuf_t *sp) {
	Free(sp->buf);
}

void sbuf_insert(sbuf_t *sp, int item) {
	P(&sp->slots);
	P(&sp->mutex);
	sp->buf[sp->rear] = item;
	sp->rear = (sp->rear + 1) % sp->n;
	V(&sp->mutex);
	V(&sp->items);
}

int sbuf_remove(sbuf_t *sp) {
	int item;
	P(&sp->items);
	P(&sp->mutex);
	item = sp->buf[sp->front];
	sp->front = (sp->front + 1) % sp->n;
	V(&sp->mutex);
	V(&sp->slots);
	return item;
}

void logbuf_init(logbuf_t *lp, int n) {
  lp->buf = Malloc(n * sizeof(char*));
  for (int i = 0; i < n; i++) {
    lp->buf[i] = Malloc(LOG_MSG_LEN * sizeof(char));
  }
  lp->n = n;
	Sem_init(&lp->mutex, 0, 1);
	Sem_init(&lp->slots, 0, n);
	Sem_init(&lp->items, 0, 0);
}

void logbuf_deinit(logbuf_t *lp) {
  for (int i = 0; i < SBUFSIZE; i++) {
    Free(lp->buf[i]);
  }
  Free(lp->buf);
}

void logbuf_insert(logbuf_t *lp, char *item) {
  P(&lp->slots);
  P(&lp->mutex);
  lp->buf[lp->rear] = item;
  lp->rear = (lp->rear + 1) % lp->n;
  V(&lp->mutex);
  V(&lp->items);
}

char *logbuf_remove(logbuf_t *lp) {
  char *item;
  P(&lp->items);
  P(&lp->mutex);
  item = lp->buf[lp->front];
  lp->front = (lp->front + 1) % lp->n;
  V(&lp->mutex);
  V(&lp->slots);
  return item;
}

void cache_buf_init(cache_buf *cache) {
	cache->head = NULL;
	cache->readcnt = 0;
	cache->writecnt = 0;
	Sem_init(&cache->outerQ, 0, 1);
	Sem_init(&cache->rsem, 0, 1);
	Sem_init(&cache->rmutex, 0, 1);
	Sem_init(&cache->wmutex, 0, 1);
	Sem_init(&cache->wsem, 0, 1);
}

/*void sigint_handler(int sig) {
  pid_t fg_pid = fgpid(jobs);
  if (fg_pid != 0) {
    kill(-fg_pid, SIGINT);
  }
  return;
}*/

void *log_thread(void *vargp) {
  FILE *logfile;
  logfile = fopen("log.txt", "w+");
  if (logfile == NULL) {
    printf("logfile doesn't exist!\n");
  }

  while(1) {
    char *msg = logbuf_remove(&logbuf);
    fprintf(logfile, "%s\n", msg);
		fflush(logfile);
  }

}

void *thread(void *vargp) {
	Pthread_detach(pthread_self());
	while(1) {
		//logbuf_insert(&logbuf, "Launched a thread\n");
		int connfd = sbuf_remove(&sbuf);
		doit(connfd);
		Close(connfd);
	}
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
	char log_msg[50]; // = "Check my logger";

	rio_t		rio;

	/* Read request line and headers */
	Rio_readinitb(&rio, fd);
	if (!Rio_readlineb(&rio, buf, MAXLINE)) {
		return;
  }
	printf("%s", buf);
	sscanf(buf, "%s %s %s", method, uri, version);
	//printf("Processing %s\n", uri);
	strcat(log_msg, "Processing ");
	strcat(log_msg, uri);
	logbuf_insert(&logbuf, log_msg);
	if (strcasecmp(method, "GET") != 0) { //if it doesn't equal GET
		char *error_msg = "Proxy only handles GET requests";
		logbuf_insert(&logbuf, error_msg);
		return;
  }
	store_requesthdrs(&rio, headers);

	int response_size = 0;
	is_proxy = check_if_proxy(uri);

	if (is_proxy) {
	  //check cache
	  cache_line *cached_resp = NULL;
	  int is_cached = read_cache(&cache, uri);

		//so if in cache...
		if (is_cached) {
			//printf("In cached\n");
			//char *log_msg = "In cached";
			//logbuf_insert(&logbuf, log_msg);
			write_cache(&cache, &cached_resp, uri);
			if (cached_resp != NULL) {
				//printf("Cached response not NULL\n");
				//printf("%s", cached_resp->data);
				response_size = cached_resp->size;
				strncpy(response, cached_resp->data, response_size);
			}
			//printf("Leaving cached\n");
		}

		//so if not in cache...parse, creat_msg, and send_msg
		else {
			parse_uri_proxy(uri, hostname, port, path);
			create_msg(path, headers, hostname, msg);
			//printf("hostname: %s\n", hostname);
			//printf("port: %s\n", port);
			//printf("path: %s\n", path);
			//printf("Request:\n");
			//printf("%s", msg);
			response_size = send_recv_message(hostname, port, msg, strlen(msg), response);

			if (response_size < MAX_OBJECT_SIZE) {
				printf("Adding to the cache\n");
				cache_line *new_obj = (cache_line *) Malloc(1 * sizeof(cache_line));
				new_obj->data = response;
				new_obj->size = response_size;
				new_obj->uri = uri;
				new_obj->next = NULL;

				write_cache(&cache, &new_obj, NULL);
			}
		}
		printf("Response:\n");
		printf("%s", response);
		Rio_writen(fd, response, response_size);

	}
	else {
		char *error_msg = "Not a valid proxy request";
		logbuf_insert(&logbuf, error_msg);
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
	printf("LENGTH = %d\n", contentLength);

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

int read_cache(cache_buf *cache, char *uri) {
	int is_cached = 0;

	P(&cache->outerQ);
	P(&cache->rsem);
	P(&cache->rmutex);
	cache->readcnt++;
	if (cache->readcnt == 1) {
		P(&cache->wsem);
	}
	V(&cache->rmutex);
	V(&cache->rsem);
	V(&cache->outerQ);

	cache_line *cached_resp = check_cache(uri);
	if (cached_resp != NULL) {
		is_cached = 1;
	}

	P(&cache->rmutex);
	cache->readcnt--;
	if (cache->readcnt == 0) {
		V(&cache->wsem);
	}
	V(&cache->rmutex);

	return is_cached;
}

void write_cache(cache_buf *cache, cache_line **response, char *uri) {
	P(&cache->wsem);
	cache->writecnt++;
	if (cache->writecnt == 1) {
		P(&cache->rsem);
	}
	V(&cache->wsem);
	P(&cache->wmutex);

	if ((*response) == NULL) { //(*response)->size == 22) { // == NULL) {
	  //printf("Original response was NULL\n");
		*response = check_cache(uri);
	}

	cache_insert(*response);

	V(&cache->wmutex);
	P(&cache->wsem);
	cache->writecnt--;
	if (cache->writecnt == 0) {
		V(&cache->rsem);
	}
	V(&cache->wsem);
	printf("Testing timing...\n");
	if (response != NULL) {
		printf("Response is NOT null at exit\n");
		printf("%s", (*response)->data);
	}
}

cache_line* check_cache(char *uri) {
  cache_line *current = cache.head;

  while (current != NULL) {
    if (strcmp(current->uri, uri) == 0) {
			return current;
    }
		current = current->next;
  }
	return NULL;
}

void cache_insert(cache_line *response) {
  if (cache.head == response) {
    return;
  }

  if ((cache_size + response->size) > MAX_CACHE_SIZE) {
    //evict
    delete_last();
  }
  //if cache is empty
	if (cache.head == NULL) {
		cache.head = response;
	}
	else {
		//if it's in the middle of the cache
		if (response->next != NULL) {
    	cache_line *current = cache.head;
    	while (current->next != response) {
				current = current->next;
    	}
			current->next = response->next;
		}
		response->next = cache.head;
		cache.head = response;
  }
	cache_size += response->size;
}

void delete_last() {
  cache_line *temp = cache.head;
  cache_line *current;
  if (cache.head == NULL) {
    return;
  }
  if (cache.head->next == NULL) {
    Free(cache.head);
    cache.head = NULL;
  }
  else {
    while (temp->next != NULL) {
      current = temp;
      temp = temp->next;
    }
    cache_size -= current->next->size;
    Free(current->next);
    current->next = NULL;
  }

}

void free_cache(cache_buf *cache) {
  cache_line *current = cache->head;
  cache_line *tmp;
  while (current != NULL) {
    tmp = current;
    Free(current);
    current = tmp->next;
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
