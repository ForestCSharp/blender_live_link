#pragma once


#if defined(_WIN32)
#define SOCKET_PLATFORM_WINDOWS 
#endif

#if defined(SOCKET_PLATFORM_WINDOWS)
	/* See http://stackoverflow.com/questions/12765743/getaddrinfo-on-win32 */
	#ifndef _WIN32_WINNT
	  #define _WIN32_WINNT 0x0501  /* Windows XP. */
	#endif
	#include <winsock2.h>
	#include <Ws2tcpip.h>
		
	#pragma comment(lib, "Ws2_32.lib")

#else

	/* Assume that any non-Windows platform uses POSIX-style sockets instead. */
	#include <sys/socket.h>
	#include <arpa/inet.h>
	#include <cerrno>
	#include <netdb.h>  /* Needed for getaddrinfo() and freeaddrinfo() */
	#include <unistd.h> /* Needed for close() */
	typedef int SOCKET;

#endif

#define SOCKET_OP(f) \
{\
	int result = (f);\
	if (result != 0)\
	{\
		printf("Line %i error on %s: %i\n", __LINE__, #f, errno);\
		exit(0);\
	}\
}

int socket_lib_init(void)
{
#if defined(SOCKET_PLATFORM_WINDOWS)
	WSADATA wsa_data;
	return WSAStartup(MAKEWORD(1,1), &wsa_data);
#else
	return 0;
#endif
}

int socket_lib_quit(void)
{
#if defined(SOCKET_PLATFORM_WINDOWS)
    return WSACleanup();
#else
    return 0;
#endif
}

SOCKET socket_open(int domain, int type, int protocol)
{
	return socket(domain, type, protocol);
}

int socket_close(SOCKET sock)
{
  int status = 0;

#if defined(SOCKET_PLATFORM_WINDOWS)
    status = shutdown(sock, SD_BOTH);
    if (status == 0) { status = closesocket(sock); }
#else
    status = shutdown(sock, SHUT_RDWR);
    if (status == 0) { status = close(sock); }
#endif

  return status;
}

size_t socket_recv(SOCKET in_socket, void* in_buffer, size_t in_len, int in_flags)
{
#if defined(SOCKET_PLATFORM_WINDOWS)
	return recv(in_socket, (char*) in_buffer, in_len, in_flags);
#else
	return recv(in_socket, in_buffer, in_len, in_flags);
#endif
}

void socket_set_recv_timeout(SOCKET in_socket, const struct timeval& in_timeval)
{
	#if defined(SOCKET_PLATFORM_WINDOWS)
		int timeout = in_timeval.tv_sec * 1000 + in_timeval.tv_usec / 1000;
		setsockopt(in_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout);
	#else
		// MAC & LINUX
		setsockopt(in_socket, SOL_SOCKET, SO_RCVTIMEO, (const void*)&in_timeval, sizeof(in_timeval));
	#endif
}

void socket_set_reuse_addr_and_port(SOCKET in_socket, bool in_enable)
{
	int optval = in_enable ? 1 : 0;
	
	#if defined(SOCKET_PLATFORM_WINDOWS)
		SOCKET_OP(setsockopt(in_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval)));
	#else
		SOCKET_OP(setsockopt(in_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)));
		SOCKET_OP(setsockopt(in_socket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)));
	#endif
}

int socket_get_last_error()
{
#if defined(SOCKET_PLATFORM_WINDOWS)
	return WSAGetLastError();
#else
	return errno;
#endif
}

int socket_error_again()
{
#if defined(SOCKET_PLATFORM_WINDOWS)
	return WSAEWOULDBLOCK;
#else
	return EAGAIN;
#endif
}

int socket_error_would_block()
{
#if defined(SOCKET_PLATFORM_WINDOWS)
	return WSAEWOULDBLOCK;
#else
	return EWOULDBLOCK;
#endif
}

int socket_error_timed_out()
{
#if defined(SOCKET_PLATFORM_WINDOWS)
	return WSAETIMEDOUT;
#else
	return ETIMEDOUT;
#endif
}
