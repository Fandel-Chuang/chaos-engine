/*
 * ChaosEngine Windows POSIX compatibility shim
 * Provides missing POSIX symbols for MSVC builds.
 * Force-included via /FI on Windows builds.
 */

#ifndef CE_WIN_COMPAT_H
#define CE_WIN_COMPAT_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <time.h>
#include <stdint.h>
#include <io.h>
#include <process.h>

/* ---- clock_gettime / CLOCK_MONOTONIC ---- */

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#endif

static inline int clock_gettime(int clk_id, struct timespec* tp) {
    (void)clk_id;
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    tp->tv_sec  = (time_t)(count.QuadPart / freq.QuadPart);
    tp->tv_nsec = (long)((count.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
}

/* ---- usleep / nanosleep ---- */

static inline int usleep(unsigned int usec) {
    Sleep(usec / 1000 == 0 ? 1 : usec / 1000);
    return 0;
}

static inline int nanosleep(const struct timespec* req, struct timespec* rem) {
    (void)rem;
    DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    Sleep(ms < 1 ? 1 : ms);
    return 0;
}

/* ---- access / F_OK ---- */

#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1
#endif
#define access _access

/* ---- ssize_t ---- */

#ifndef ssize_t
typedef intptr_t ssize_t;
#endif

/* ---- Suppress MSVC unsafe-function warnings ---- */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

/* ---- SIGTERM (not defined on MSVC by default) ---- */

#ifndef SIGTERM
#define SIGTERM 15
#endif

/* ---- Winsock2 / POSIX socket compatibility ---- */
/* Include this before any sys/socket.h references on Windows */
#include <winsock2.h>
#include <ws2tcpip.h>

/* Map POSIX socket errno names to WSA equivalents */
#ifndef EWOULDBLOCK
#define EWOULDBLOCK  WSAEWOULDBLOCK
#endif
#ifndef EINPROGRESS
#define EINPROGRESS  WSAEINPROGRESS
#endif
#ifndef ECONNRESET
#define ECONNRESET   WSAECONNRESET
#endif
#ifndef ENOTCONN
#define ENOTCONN     WSAENOTCONN
#endif

/* MSG_NOSIGNAL — Linux-only (prevents SIGPIPE), not needed on Windows */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* POSIX close() → closesocket() for socket fds */
#define close(s) closesocket(s)

/* fcntl / O_NONBLOCK stub — use ioctlsocket instead */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x0004
#endif
static inline int fcntl_set_nonblock(SOCKET s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode);
}

#endif /* _WIN32 */
#endif /* CE_WIN_COMPAT_H */
