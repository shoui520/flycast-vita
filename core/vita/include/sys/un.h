// Minimal sys/un.h for the PS Vita newlib, which has none.
// Local sockets aren't supported by the Vita.
#pragma once
#include <sys/socket.h>

struct sockaddr_un {
	sa_family_t sun_family;
	char sun_path[104];
};
