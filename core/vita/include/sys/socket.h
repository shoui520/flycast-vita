// Additions to the PS Vita sys/socket.h needed to build asio.
#pragma once
#include_next <sys/socket.h>

#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif

#ifdef __cplusplus
extern "C" {
#endif
int sockatmark(int fd);
#ifdef __cplusplus
}
#endif
