#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#define inet_ntoa_r(addr,buf,len) inet_ntop(AF_INET,&(addr),(buf),(len))
