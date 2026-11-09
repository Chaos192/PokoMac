// SPDX-License-Identifier: GPL-3.0-or-later
// nxwin <iphlpapi.h>: GetAdaptersAddresses over getifaddrs (adapter name, operational state, loopback type, unicast addresses with
// their prefix length). Only the fields below are filled.
#pragma once
#include "winsock2.h"
#define ERROR_BUFFER_OVERFLOW 111L
#define ERROR_NO_DATA 232L
#define GAA_FLAG_SKIP_UNICAST 0x0001
#define GAA_FLAG_SKIP_ANYCAST 0x0002
#define GAA_FLAG_SKIP_MULTICAST 0x0004
#define GAA_FLAG_SKIP_DNS_SERVER 0x0008
#define GAA_FLAG_INCLUDE_PREFIX 0x0010
#define GAA_FLAG_SKIP_FRIENDLY_NAME 0x0020
#define GAA_FLAG_INCLUDE_GATEWAYS 0x0080
#define IF_TYPE_OTHER 1
#define IF_TYPE_ETHERNET_CSMACD 6
#define IF_TYPE_SOFTWARE_LOOPBACK 24
#define IF_TYPE_IEEE80211 71
#define IF_TYPE_TUNNEL 131
typedef enum { IfOperStatusUp = 1, IfOperStatusDown, IfOperStatusTesting, IfOperStatusUnknown, IfOperStatusDormant, IfOperStatusNotPresent,
               IfOperStatusLowerLayerDown } IF_OPER_STATUS;
typedef struct _SOCKET_ADDRESS { struct sockaddr* lpSockaddr; INT iSockaddrLength; } SOCKET_ADDRESS;
typedef struct _IP_ADAPTER_UNICAST_ADDRESS {
    ULONG Length; DWORD Flags;
    struct _IP_ADAPTER_UNICAST_ADDRESS* Next;
    SOCKET_ADDRESS Address;
    UINT8 OnLinkPrefixLength;
} IP_ADAPTER_UNICAST_ADDRESS, *PIP_ADAPTER_UNICAST_ADDRESS;
typedef struct _IP_ADAPTER_ADDRESSES {
    ULONG Length; DWORD IfIndex;
    struct _IP_ADAPTER_ADDRESSES* Next;
    char* AdapterName;
    PIP_ADAPTER_UNICAST_ADDRESS FirstUnicastAddress;
    void* FirstAnycastAddress;
    void* FirstMulticastAddress;
    void* FirstDnsServerAddress;
    wchar_t* DnsSuffix;
    wchar_t* Description;
    wchar_t* FriendlyName;
    BYTE PhysicalAddress[8];
    DWORD PhysicalAddressLength;
    DWORD Flags;
    DWORD Mtu;
    DWORD IfType;
    IF_OPER_STATUS OperStatus;
} IP_ADAPTER_ADDRESSES, *PIP_ADAPTER_ADDRESSES;
ULONG GetAdaptersAddresses(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES out, PULONG len);
