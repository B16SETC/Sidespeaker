#include "net.h"

bool sendAll(sock_t s, const void* b, int n) {
    const char* p = (const char*)b;
    while (n > 0) {
        int r = (int)send(s, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

bool recvAll(sock_t s, void* b, int n) {
    char* p = (char*)b;
    while (n > 0) {
        int r = (int)recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

bool waitReadable(sock_t s, int ms) {
    fd_set f;
    FD_ZERO(&f);
    FD_SET(s, &f);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select((int)s + 1, &f, nullptr, nullptr, &tv) > 0;
}

sock_t connectLocal(int port) {
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSock) return kBadSock;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(s, (sockaddr*)&a, sizeof a) != 0) {
        closeSock(s);
        return kBadSock;
    }
    return s;
}
