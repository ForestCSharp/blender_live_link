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
	#include <fcntl.h>
	#include <netdb.h>  /* Needed for getaddrinfo() and freeaddrinfo() */
	#include <sys/select.h>
	#include <unistd.h> /* Needed for close() */
	typedef int SOCKET;
	#define INVALID_SOCKET -1
#endif

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
	SOCKET new_socket = socket(domain, type, protocol);

#if defined(SOCKET_PLATFORM_WINDOWS)
	// Change IO mode of socket to nonblocking (mode == 1)
	u_long mode_nonblocking = 1;
	ioctlsocket(new_socket, FIONBIO, &mode_nonblocking);
#else
	// Add O_NONBLOCK flag to file descriptor
	int flags = fcntl(new_socket, F_GETFL, 0);
	fcntl(new_socket, F_SETFL, flags | O_NONBLOCK);
#endif

	return new_socket;
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

SOCKET socket_invalid(void)
{
	return INVALID_SOCKET;	
}

bool socket_is_valid(SOCKET sock)
{
	return sock != INVALID_SOCKET;
}

size_t socket_recv(SOCKET in_socket, void* in_buffer, size_t in_len, int in_flags)
{
#if defined(SOCKET_PLATFORM_WINDOWS)
	return recv(in_socket, (char*) in_buffer, in_len, in_flags);
#else
	return recv(in_socket, in_buffer, in_len, in_flags);
#endif
}

// Sleeps until in_socket is readable (data, EOF, or a pending accept) or the
// timeout passes. Lets nonblocking sockets idle without spinning a core.
bool socket_wait_readable(SOCKET in_socket, int in_timeout_ms)
{
	fd_set read_set;
	FD_ZERO(&read_set);
	FD_SET(in_socket, &read_set);
	struct timeval timeout = {.tv_sec = in_timeout_ms / 1000, .tv_usec = (in_timeout_ms % 1000) * 1000};
	return select((int) in_socket + 1, &read_set, nullptr, nullptr, &timeout) > 0;
}

int socket_get_last_error();

// SO_REUSEADDR only: it lets a restart rebind a port still held by our own
// TIME_WAIT sockets. SO_REUSEPORT is intentionally not set — on macOS/BSD it
// would let a second instance bind the same listening port and silently take
// over incoming connections instead of failing with EADDRINUSE.
void socket_set_reuse_addr(SOCKET in_socket, bool in_enable)
{
	int optval = in_enable ? 1 : 0;

	#if defined(SOCKET_PLATFORM_WINDOWS)
		const int result = setsockopt(in_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));
	#else
		const int result = setsockopt(in_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	#endif

	// Non-fatal: without it a rebind may hit a lingering TIME_WAIT socket,
	// which the caller already reports.
	if (result != 0)
	{
		printf("socket: SO_REUSEADDR failed (error %i)\n", socket_get_last_error());
	}
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

int socket_error_addr_in_use()
{
#if defined(SOCKET_PLATFORM_WINDOWS)
	return WSAEADDRINUSE;
#else
	return EADDRINUSE;
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
