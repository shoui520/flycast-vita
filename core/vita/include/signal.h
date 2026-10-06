// Additions to the PS Vita signal.h needed to build asio.
#pragma once
#include_next <signal.h>

#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif
#ifndef SA_NOCLDWAIT
#define SA_NOCLDWAIT 0x00000020
#endif
