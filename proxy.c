#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "csapp.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define NTHREADS 4
#define SBUFSIZE 16

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

struct Uri
{
    char host[MAXLINE]; //主机名
    char port[MAXLINE]; //端口
    char path[MAXLINE]; //路径
};

/* ---------------- 有界缓冲（生产者-消费者） ---------------- */
typedef struct{
	int *buf;
	int n;
	int first;   //buf[(first+1)%n]为第一项
	int last;    //buf[last%n] lastone
	sem_t mutex;
	sem_t place;
	sem_t items;
}sbuf_t;

static sbuf_t sbuf;   // 有界缓冲（全局，供主线程与工作线程共享）

void sbuf_init(sbuf_t *sp, int n)
{
	sp->buf = Calloc(n, sizeof(int));
	sp->n = n;
	sp->first = sp->last = 0;
	Sem_init(&sp->mutex, 0, 1);
	Sem_init(&sp->place, 0, n);
	Sem_init(&sp->items, 0, 0);
}

void sbuf_insert(sbuf_t *sp, int item)
{
	P(&sp->place);  //wait for available slot
	P(&sp->mutex);
	sp->buf[(++sp->last)%(sp->n)] = item; //insert
	V(&sp->mutex);
	V(&sp->items);
}

int sbuf_remove(sbuf_t *sp)
{
	int item;
	P(&sp->items);
	P(&sp->mutex);
	item = sp->buf[(++sp->first)%(sp->n)];
	V(&sp->mutex);
	V(&sp->place);
	return item;
}

typedef struct CacheBlock {
    char url[MAXLINE];          
    char *object;               //缓存的完整响应
    int size;                   //响应字节数
    struct CacheBlock *prev;    //前驱
    struct CacheBlock *next;    //后
} CacheBlock;

typedef struct {
    CacheBlock *head;          
    CacheBlock *tail;           
    int total_size;             
    sem_t mutex;                
} Cache;

static Cache cache;

void cache_init(Cache *c)
{
    c->head = c->tail = NULL;
    c->total_size = 0;
    Sem_init(&c->mutex, 0, 1);
}


int cache_lookup(Cache *c, const char *url, char *out)
{
    int size = -1;
    P(&c->mutex);
    CacheBlock *p;
    for (p = c->head; p != NULL; p = p->next) {
        if (strcmp(p->url, url) == 0) {
            size = p->size;
            memcpy(out, p->object, size);
            if (p != c->head) {         
                p->prev->next = p->next;
                if (p->next) p->next->prev = p->prev;
                else         c->tail = p->prev;
                p->prev = NULL;
                p->next = c->head;
                c->head->prev = p;
                c->head = p;
            }
            break;
        }
    }
    V(&c->mutex);
    return size;
}


void cache_insert(Cache *c, const char *url, char *object, int size)
{
    if (size > MAX_OBJECT_SIZE)     //单个对象超限，不缓存
        return;

    P(&c->mutex);

    
    CacheBlock *p;
    for (p = c->head; p != NULL; p = p->next) {
        if (strcmp(p->url, url) == 0) {
            if (p->prev) p->prev->next = p->next; else c->head = p->next;
            if (p->next) p->next->prev = p->prev; else c->tail = p->prev;
            c->total_size -= p->size;
            Free(p->object);
            Free(p);
            break;
        }
    }

    // 头插新块 
    CacheBlock *nb = Malloc(sizeof(CacheBlock));
    strncpy(nb->url, url, MAXLINE - 1);
    nb->url[MAXLINE - 1] = '\0';
    nb->object = Malloc(size);
    memcpy(nb->object, object, size);
    nb->size = size;
    nb->prev = NULL;
    nb->next = c->head;
    if (c->head) c->head->prev = nb;
    c->head = nb;
    if (c->tail == NULL) c->tail = nb;
    c->total_size += size;

    // 超出总容量
    while (c->total_size > MAX_CACHE_SIZE && c->tail != NULL) {
        CacheBlock *victim = c->tail;
        c->tail = victim->prev;
        if (c->tail) c->tail->next = NULL;
        else         c->head = NULL;
        c->total_size -= victim->size;
        Free(victim->object);
        Free(victim);
    }

    V(&c->mutex);
}


void *thread(void *vargp);
void doit(int connfd);
void parse_uri(char *uri, struct Uri *uri_data);

void sigpipe_handler(int sig)
{
    (void)sig;
}

int main(int argc, char **argv)
{
    int listenfd, connfd;
    socklen_t clientlen;
    char hostname[MAXLINE], port[MAXLINE];

    struct sockaddr_storage clientaddr;

    pthread_t tid;

    if (argc != 2)
    {
        fprintf(stderr, "usage :%s <port> \n", argv[0]);
        exit(1);
    }
    signal(SIGPIPE, sigpipe_handler);	//捕获SIGPIPE信号
    listenfd = Open_listenfd(argv[1]);

    sbuf_init(&sbuf, SBUFSIZE);
    cache_init(&cache);
    for(int i = 0; i < NTHREADS; i++)
    {
	    Pthread_create(&tid,NULL,thread,NULL);
    }

    while (1)
    {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        sbuf_insert(&sbuf, connfd);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s %s).\n", hostname, port);
    }
    return 0;
}

void *thread(void *vargp)
{
    Pthread_detach(pthread_self());
    while(1){
    int connfd = sbuf_remove(&sbuf);
    doit(connfd);
    Close(connfd);
    }
}


void doit(int connfd)
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char cache_key[MAXLINE];
    char request[4 * MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, connfd);
    if (Rio_readlineb(&rio, buf, MAXLINE) <= 0)
    {
        return;
    }

    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET") != 0)
    {
        printf("Proxy does not implement the method\n");
        return;
    }

    strcpy(cache_key, uri);     
    struct Uri uri_data;
    memset(&uri_data, 0, sizeof(uri_data));
    parse_uri(uri, &uri_data);

    //消费掉客户端剩余的请求
    while (strcmp(buf, "\r\n") != 0)
    {
        if (Rio_readlineb(&rio, buf, MAXLINE) <= 0)
            return;             
    }

    
    char *cached = Malloc(MAX_OBJECT_SIZE);
    int cached_size = cache_lookup(&cache, cache_key, cached);
    if (cached_size > 0)
    {
        rio_writen(connfd, cached, cached_size);
        Free(cached);
        return;
    }
    Free(cached);

   
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: %s:%s\r\n"
             "%s"
             "Connection: close\r\n"
             "Proxy-Connection: close\r\n"
             "\r\n",
             uri_data.path, uri_data.host, uri_data.port, user_agent_hdr);

    int serverfd = open_clientfd(uri_data.host, uri_data.port);
    if (serverfd < 0)
    {
        fprintf(stderr, "Failed to connect to %s:%s\n", uri_data.host, uri_data.port);
        return;
    }

    rio_writen(serverfd, request, strlen(request));

   
    char *object = Malloc(MAX_OBJECT_SIZE);
    int object_size = 0;
    int too_big = 0;
    ssize_t n;
    while ((n = rio_readn(serverfd, buf, MAXLINE)) > 0)
    {
        if (rio_writen(connfd, buf, n) < 0)   
            break;
        if (!too_big)
        {
            if (object_size + n <= MAX_OBJECT_SIZE)
            {
                memcpy(object + object_size, buf, n);
                object_size += n;
            }
            else
            {
                too_big = 1;              
            }
        }
    }
    Close(serverfd);

    if (!too_big && object_size > 0)
        cache_insert(&cache, cache_key, object, object_size);
    Free(object);
}

/*
 * parse_uri - 把 "http://host[:port][/path]" 拆成 host、port、path 三部分
 */
void parse_uri(char *uri, struct Uri *uri_data)
{
    char *host_start = strstr(uri, "//");
    char *path_start;
    char *port_start;

    memset(uri_data->host, 0, MAXLINE);
    memset(uri_data->port, 0, MAXLINE);
    memset(uri_data->path, 0, MAXLINE);

    if (host_start == NULL)
    {
        strcpy(uri_data->port, "80");
        path_start = strstr(uri, "/");
        if (path_start != NULL)
        {
            strcpy(uri_data->path, path_start);
        }
        return;
    }

    host_start += 2;
    port_start = strchr(host_start, ':');
    path_start = strchr(host_start, '/');

    if (port_start != NULL && (path_start == NULL || port_start < path_start))
    {
        char portbuf[32];
        int portnum;
        sscanf(port_start + 1, "%d", &portnum);
        snprintf(portbuf, sizeof(portbuf), "%d", portnum);
        strcpy(uri_data->port, portbuf);
        *port_start = '\0';
    }
    else
    {
        strcpy(uri_data->port, "80");
    }

    if (path_start != NULL)
    {
        strcpy(uri_data->path, path_start);
        *path_start = '\0';
    }

    strcpy(uri_data->host, host_start);
}
