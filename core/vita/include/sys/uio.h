// Minimal sys/uio.h for the PS Vita newlib, which has none.
#pragma once
#include <sys/types.h>
#include <sys/socket.h>	// struct iovec

#ifdef __cplusplus
extern "C" {
#endif
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
#ifdef __cplusplus
}
#endif
