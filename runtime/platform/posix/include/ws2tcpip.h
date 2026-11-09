#pragma once
#include "winsock2.h"
typedef struct addrinfo ADDRINFOW;
typedef struct addrinfo* PADDRINFOA;
#define InetPtonA inet_pton
#define InetNtopA inet_ntop
