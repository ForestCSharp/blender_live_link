#pragma once

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
	#error "Missing Sockets Support for Windows"	
#else
	#include <unistd.h>
	#include <sys/socket.h>
	#include <netdb.h>
#endif
