// Host shim: OpenOrbis sceNet* over BSD sockets (IPv4 literals only -- the
// test server is 127.0.0.1, so the resolver is never needed).
#pragma once
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0      /* macOS: the driver ignores SIGPIPE instead */
#endif
typedef int OrbisNetId;
typedef struct { uint32_t s_addr; } OrbisNetInAddr;
typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint8_t zero[8]; } OrbisNetSockaddr;
#define ORBIS_NET_AF_INET 2
#define ORBIS_NET_SOCK_STREAM 1
#define ORBIS_NET_SO_NBIO 0x1200
static inline int sceNetSocket(const char*n,int d,int t,int p){(void)n;(void)d;(void)t;(void)p;return socket(AF_INET,SOCK_STREAM,0);}
static inline int sceNetSocketClose(int s){return close(s);}
static inline int sceNetSocketAbort(int s,int f){(void)f;return shutdown(s,SHUT_RDWR);}
static inline int sceNetSetsockopt(int s,int lvl,int opt,const void*v,unsigned l){(void)lvl;(void)l;
  if(opt==0x1005||opt==0x1006){int us=*(const int*)v;struct timeval tv={us/1000000,us%1000000};return setsockopt(s,SOL_SOCKET,opt==0x1006?SO_RCVTIMEO:SO_SNDTIMEO,&tv,sizeof tv);}
  if(opt==ORBIS_NET_SO_NBIO){int fl=fcntl(s,F_GETFL);return fcntl(s,F_SETFL,*(const int*)v?fl|O_NONBLOCK:fl&~O_NONBLOCK);}
  return 0;}
static inline int sceNetConnect(int s,const OrbisNetSockaddr*a,unsigned l){(void)l;struct sockaddr_in sin;memset(&sin,0,sizeof sin);sin.sin_family=AF_INET;sin.sin_port=a->port;sin.sin_addr.s_addr=a->addr;return connect(s,(struct sockaddr*)&sin,sizeof sin);}
static inline int sceNetRecv(int s,void*b,unsigned l,int f){return (int)recv(s,b,l,f);}
static inline int sceNetSend(int s,const void*b,unsigned l,int f){return (int)send(s,b,l,f|MSG_NOSIGNAL);}
static inline uint16_t sceNetHtons(uint16_t v){return htons(v);}
static inline int sceNetInetPton(int af,const char*src,void*dst){(void)af;return inet_pton(AF_INET,src,dst);}
static inline int sceNetPoolCreate(const char*n,int sz,int f){(void)n;(void)sz;(void)f;return 1;}
static inline OrbisNetId sceNetResolverCreate(const char*n,int p,int f){(void)n;(void)p;(void)f;return -1;}
static inline int sceNetResolverStartNtoa(OrbisNetId r,const char*h,OrbisNetInAddr*a,int t,int rt,int f){(void)r;(void)h;(void)a;(void)t;(void)rt;(void)f;return -1;}
static inline int sceNetResolverDestroy(OrbisNetId r){(void)r;return 0;}
