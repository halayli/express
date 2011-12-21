#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <lthread.h>

#include <sys/un.h>

int
e_listener(char *ip, short port)
{
	int fd = 0, ret = 0;
	int opt = 1;
	struct   sockaddr_in sin;

	fd = lthread_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (!fd) 
		return -1;

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt,sizeof(int)) == -1) {
		perror("failed to set SOREUSEADDR on socket");
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = PF_INET;
	sin.sin_addr.s_addr = INADDR_ANY;
	sin.sin_port = htons(port);
	ret = bind(fd, (struct sockaddr *)&sin, sizeof(sin));
	if (ret == -1) {
		close(fd);
		perror("cannot bind socket. closing.\n");
		return -1;
	}
	ret = listen(fd, 2000);
	if (ret == -1) {
		close(fd);
		perror("cannot listen on socket. closing.\n");
		return -1;
	}

	return fd;
}

int
e_local_listener(char *path)
{
	int fd = 0, ret = 0;
	struct   sockaddr_un name;

	fd = lthread_socket(PF_LOCAL, SOCK_STREAM, 0);
	if (!fd) 
		return -1;

	memset(&name, 0, sizeof(name));
	name.sun_family = PF_LOCAL;
	strncpy(name.sun_path, path, sizeof (name.sun_path));
	name.sun_path[strlen(path) + 1] = '\0';
	ret = bind(fd, (struct sockaddr *)&name, SUN_LEN(&name));
	if (ret == -1) {
		close(fd);
		perror("cannot bind socket. closing.\n");
		return -1;
	}
	ret = listen(fd, 1500);
	if (ret == -1) {
		close(fd);
		perror("cannot listen on socket. closing.\n");
		return -1;
	}

	return fd;
}

int
e_local_connect(lthread_t *lt, char *path)
{
	int fd = 0;
	struct sockaddr_un name;

	fd = lthread_socket(PF_LOCAL, SOCK_STREAM, 0);
	if (!fd) 
		return -1;

	memset(&name, 0, sizeof(name));
	name.sun_family = PF_LOCAL;
	strncpy(name.sun_path, path, sizeof (name.sun_path));
	name.sun_path[sizeof (name.sun_path) - 1] = '\0';

	lthread_connect(lt, fd, (struct sockaddr *)&name, SUN_LEN(&name), 1);

	return fd;
}
