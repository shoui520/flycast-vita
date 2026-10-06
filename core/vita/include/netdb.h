// Additions to the PS Vita netdb.h needed to build asio.
#pragma once
#include_next <netdb.h>

#ifndef HOST_NOT_FOUND
#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4
#endif
