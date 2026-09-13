#include "csapp.h"
#include <strings.h>

#define MAX_CACHE_SIZE 1048576
#define MAX_OBJECT_SIZE 102400

typedef struct CCB
{
    char uri[MAXLINE];
    char *data;
    int size;
    unsigned long time;
    struct CCB *next;
} CCB;

CCB *cache=NULL;
int cache_size=0;
unsigned long cache_time=0;
pthread_rwlock_t cache_lock=PTHREAD_RWLOCK_INITIALIZER;
pthread_mutex_t time_lock=PTHREAD_MUTEX_INITIALIZER;

static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
    "Gecko/20120305 Firefox/10.0.3\r\n";

void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *port, char *path);
void *thread(void *vargp);
int cache_read(char *uri, char *data);
void cache_write(char *uri, char *data, int size);

int main(int argc, char **argv)
{
    int listenfd, *connfdp;
    pthread_t tid;
    struct sockaddr_storage clientaddr;
    socklen_t clientlen;
    if (argc != 2) 
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    listenfd=Open_listenfd(argv[1]);
    while (1) 
    {
        clientlen = sizeof(clientaddr);
        connfdp=Malloc(sizeof(int));
        *connfdp=Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Pthread_create(&tid,NULL,thread,connfdp);
    }
}

void *thread(void *vargp)
{
    int connfd=*((int *)vargp);
    Pthread_detach(Pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

void doit(int connfd)
{
    int serverfd;
    int has_host=0;
    int size=0;
    ssize_t n;
    char buf[MAXLINE];
    char method[MAXLINE];
    char uri[MAXLINE];
    char version[MAXLINE];
    char key[MAXLINE];
    char object[MAX_OBJECT_SIZE];

    char hostname[MAXLINE];
    char port[16];
    char path[MAXLINE];

    rio_t client_rio;
    rio_t server_rio;

    Rio_readinitb(&client_rio, connfd);
    if (Rio_readlineb(&client_rio, buf, MAXLINE) <= 0)
        return;
    sscanf(buf,"%s %s %s",method,uri,version);

    if (strcmp(method,"GET"))
        return;
    strcpy(key,uri);
    size=cache_read(key,object);
    if (size)
    {
        while (Rio_readlineb(&client_rio,buf,MAXLINE)>0)
        {
            if (!strcmp(buf,"\r\n"))
                break;
        }
        rio_writen(connfd,object,size);
        return;
    }
    parse_uri(uri,hostname,port,path);

    serverfd=open_clientfd(hostname,port);
    if (serverfd<0)
        return;
    sprintf(buf, "GET %s HTTP/1.0\r\n", path);
    rio_writen(serverfd,buf,strlen(buf));

    while (Rio_readlineb(&client_rio,buf,MAXLINE)>0) 
    {
        if (!strcmp(buf, "\r\n"))
            break;
        if (!strncmp(buf, "Host:", 5)) 
        {
            has_host=1;
            rio_writen(serverfd, buf, strlen(buf));
        }
        else if (!strncmp(buf, "User-Agent:", 11)||!strncmp(buf, "Connection:", 11) ||!strncmp(buf, "Proxy-Connection:", 17)) 
            continue;
        else 
            rio_writen(serverfd, buf, strlen(buf));
    }

    if (!has_host) 
    {
        if(!strcmp(port, "80"))
            sprintf(buf, "Host: %s\r\n", hostname);
        else
            sprintf(buf, "Host: %s:%s\r\n", hostname, port);
        rio_writen(serverfd, buf, strlen(buf));
    }

    rio_writen(serverfd, (void *)user_agent_hdr, strlen(user_agent_hdr));
    rio_writen(serverfd, "Connection: close\r\n",strlen("Connection: close\r\n"));
    rio_writen(serverfd, "Proxy-Connection: close\r\n",strlen("Proxy-Connection: close\r\n"));
    rio_writen(serverfd, "\r\n", 2);
    Rio_readinitb(&server_rio, serverfd);
    while ((n = rio_readnb(&server_rio, buf, MAXBUF))>0)
    {
        rio_writen(connfd, buf, n);
        if (size>=0)
        {
            if (size+n<=MAX_OBJECT_SIZE)
            {
                memcpy(object+size,buf,n);
                size+=n;
            }
            else
                size=-1;
        }
    }
    Close(serverfd);
    if (size>0 && n==0)
        cache_write(key,object,size);
}

int cache_read(char *uri, char *data)
{
    CCB *p;
    int size=0;
    pthread_rwlock_rdlock(&cache_lock);
    for (p=cache;p;p=p->next)
    {
        if (!strcmp(p->uri,uri))
        {
            pthread_mutex_lock(&time_lock);
            p->time=++cache_time;
            pthread_mutex_unlock(&time_lock);
            size=p->size;
            memcpy(data,p->data,size);
            break;
        }
    }
    pthread_rwlock_unlock(&cache_lock);
    return size;
}

void cache_write(char *uri, char *data, int size)
{
    CCB *p;
    CCB **pos, **old;
    pthread_rwlock_wrlock(&cache_lock);
    for (p=cache;p;p=p->next)
    {
        if (!strcmp(p->uri,uri))
        {
            p->time=++cache_time;
            pthread_rwlock_unlock(&cache_lock);
            return;
        }
    }
    while (cache_size+size>MAX_CACHE_SIZE)
    {
        old=&cache;
        for (pos=&cache;*pos;pos=&(*pos)->next)
        {
            if ((*pos)->time<(*old)->time)
                old=pos;
        }
        p=*old;
        *old=p->next;
        cache_size-=p->size;
        Free(p->data);
        Free(p);
    }
    p=Malloc(sizeof(CCB));
    strcpy(p->uri,uri);
    p->data=Malloc(size);
    memcpy(p->data,data,size);
    p->size=size;
    p->time=++cache_time;
    p->next=cache;
    cache=p;
    cache_size+=size;
    pthread_rwlock_unlock(&cache_lock);
}

void parse_uri(char *uri, char *hostname, char *port, char *path)
{
    char *host;
    char *papos;
    char *popos;
    host=uri+7;
    papos=strchr(host,'/');
    if (papos) 
    {
        strcpy(path,papos);
        *papos='\0';
    }
    else 
        strcpy(path,"/");
    popos=strchr(host,':');
    if (popos) 
    {
        *popos='\0';
        strcpy(port,popos+1);
    }
    else 
        strcpy(port,"80");
    strcpy(hostname,host);
    return;
}
