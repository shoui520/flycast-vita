// Minimal net/if.h for the PS Vita newlib, which has none.
#pragma once

#define IFNAMSIZ 16
#define IF_NAMESIZE IFNAMSIZ

#ifdef __cplusplus
extern "C" {
#endif
unsigned int if_nametoindex(const char *ifname);
char *if_indextoname(unsigned int ifindex, char *ifname);
#ifdef __cplusplus
}
#endif
