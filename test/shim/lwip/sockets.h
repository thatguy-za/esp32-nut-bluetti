#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#define inet_ntoa_r(addr,buf,len) inet_ntop(AF_INET,&(addr),(buf),(len))

/* lwIP spells the keepalive idle time TCP_KEEPIDLE, as Linux does; macOS
 * calls the same option TCP_KEEPALIVE. The host suite only needs it to
 * compile and to be accepted by setsockopt. */
#if !defined(TCP_KEEPIDLE) && defined(TCP_KEEPALIVE)
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif
