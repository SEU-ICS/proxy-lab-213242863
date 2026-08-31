#include <stdio.h>
#include "csapp.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

struct Uri
{
    char host[MAXLINE]; //hostname
    char port[MAXLINE]; //端口
    char path[MAXLINE]; //路径
};

void doit(int connfd);
void build_header(char *server, struct Uri *uri_data, rio_t *rio);
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
    if (argc != 2)
    {
        fprintf(stderr, "usage :%s <port> \n", argv[0]);
        exit(1);
    }
    signal(SIGPIPE, sigpipe_handler);	//捕获SIGPIPE信号
    listenfd = Open_listenfd(argv[1]);
    while (1)
    {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s %s).\n", hostname, port);

        doit(connfd);
        //关闭客户端的连接描述符
        Close(connfd);
    }
    return 0;
}

void build_header(char *server, struct Uri *uri_data, rio_t *rio)
{
    (void)rio;
    snprintf(server, MAXLINE,
             "GET %s HTTP/1.0\r\n"
             "Host: %s:%s\r\n"
             "User-Agent: Mozilla/5.0\r\n"
             "Connection: close\r\n"
             "\r\n",
             uri_data->path,
             uri_data->host,
             uri_data->port);
}

void doit(int connfd)
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char server[MAXLINE];
    rio_t rio, server_rio;

    Rio_readinitb(&rio, connfd);
    if (Rio_readlineb(&rio, buf, MAXLINE) == 0)
    {
        return;
    }

    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET") != 0)
    {
        printf("Proxy does not implement the method\n");
        return;
    }

    struct Uri uri_data;
    memset(&uri_data, 0, sizeof(uri_data));
    parse_uri(uri, &uri_data);

    build_header(server, &uri_data, &rio);

    int serverfd = Open_clientfd(uri_data.host, uri_data.port);
    if (serverfd < 0)
    {
        printf("connection failed\n");
        return;
    }

    Rio_readinitb(&server_rio, serverfd);
    Rio_writen(serverfd, server, strlen(server));

    size_t n;
    while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0)
    {
        printf("proxy received %d bytes,then send\n", (int)n);
        Rio_writen(connfd, buf, n);
    }

    Close(serverfd);
}

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

