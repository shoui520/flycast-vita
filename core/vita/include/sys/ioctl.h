// Minimal sys/ioctl.h for the PS Vita newlib, which has none.
// Only the socket requests used by asio are supported.
#pragma once

#define FIONBIO  0x8004667e
#define FIONREAD 0x4004667f
#define SIOCATMARK 0x40047307

#ifdef __cplusplus
extern "C" {
#endif
int ioctl(int fd, unsigned long request, ...);
#ifdef __cplusplus
}
#endif
