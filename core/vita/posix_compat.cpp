// POSIX functions missing from the PS Vita newlib
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <net/if.h>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <unistd.h>
#include <psp2/kernel/threadmgr.h>

extern "C" {

int ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	va_start(args, request);
	int *arg = va_arg(args, int *);
	va_end(args);

	switch (request)
	{
	case FIONBIO:
		return setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, arg, sizeof(int));
	case FIONREAD:
		{
			char buf[4096];
			ssize_t rc = recv(fd, buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT);
			if (rc < 0)
			{
				if (errno != EAGAIN && errno != EWOULDBLOCK)
					return -1;
				rc = 0;
			}
			*arg = (int)rc;
			return 0;
		}
	case SIOCATMARK:
		*arg = 0;
		return 0;
	default:
		errno = ENOTTY;
		return -1;
	}
}

int sockatmark(int fd)
{
	return 0;
}

unsigned int if_nametoindex(const char *ifname)
{
	errno = ENXIO;
	return 0;
}

char *if_indextoname(unsigned int ifindex, char *ifname)
{
	errno = ENXIO;
	return nullptr;
}

int pause()
{
	for (;;)
		sceKernelDelayThread(1000000);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
	ssize_t total = 0;
	for (int i = 0; i < iovcnt; i++)
	{
		ssize_t rc = read(fd, iov[i].iov_base, iov[i].iov_len);
		if (rc < 0)
			return total > 0 ? total : rc;
		total += rc;
		if ((size_t)rc < iov[i].iov_len)
			break;
	}
	return total;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
	ssize_t total = 0;
	for (int i = 0; i < iovcnt; i++)
	{
		ssize_t rc = write(fd, iov[i].iov_base, iov[i].iov_len);
		if (rc < 0)
			return total > 0 ? total : rc;
		total += rc;
		if ((size_t)rc < iov[i].iov_len)
			break;
	}
	return total;
}

}
