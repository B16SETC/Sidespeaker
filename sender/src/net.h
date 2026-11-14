#pragma once
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
constexpr sock_t kBadSock = INVALID_SOCKET;
inline void closeSock(sock_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using sock_t = int;
constexpr sock_t kBadSock = -1;
inline void closeSock(sock_t s) { close(s); }
#endif

bool sendAll(sock_t s, const void* b, int n);
bool recvAll(sock_t s, void* b, int n);
bool waitReadable(sock_t s, int ms);
sock_t connectLocal(int port);
