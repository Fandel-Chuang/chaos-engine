/* Windows stub for fcntl.h */
#pragma once
#ifdef _WIN32
/* fcntl flags — not used directly, use ioctlsocket for O_NONBLOCK */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x0004
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
static inline int fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }
#endif
