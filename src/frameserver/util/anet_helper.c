#include <arcan_shmif.h>
#include <arcan_shmif_server.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <ctype.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "a12.h"
#include "anet_helper.h"

int anet_clfd(struct addrinfo* addr)
{
	int clfd;

/* there might be many possible candidates, try them all */
	for (struct addrinfo* cur = addr; cur; cur = cur->ai_next){
		clfd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
		if (-1 == clfd)
			continue;

		if (connect(clfd, cur->ai_addr, cur->ai_addrlen) != -1){
			char hostaddr[NI_MAXHOST];
			char hostport[NI_MAXSERV];
			int ec = getnameinfo(cur->ai_addr, cur->ai_addrlen,
				hostaddr, sizeof(hostaddr), hostport, sizeof(hostport),
				NI_NUMERICSERV | NI_NUMERICHOST
			);
			int optval = 1;
			setsockopt(clfd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
			break;
		}

		close(clfd);
		clfd = -1;
	}

	return clfd;
}

struct anet_cl_connection anet_cl_setup(struct anet_options* arg)
{
	struct anet_cl_connection res = {
		.fd = -1
	};

/* open the keystore and iteratively invoke cl_setup on each entry until
 * we get a working connection */
	if (arg->key){
		if (!a12helper_keystore_open(&arg->keystore)){
			res.errmsg = strdup("couldn't open keystore");
			return res;
		}

		size_t i = 0;
		uint8_t privkey[32];
		arg->key = NULL;

		char* host;
		uint16_t port;
		res.errmsg = strdup("no matching keys for tag");

/* the cl_setup call will set errmsg on connection failure, so that need to be
 * cleaned up except for the last entry where we propagate any error message to
 * the caller */
		while (a12helper_keystore_hostkey(arg->key, i++, privkey, &host, &port)){
			if (res.errmsg){
				free(res.errmsg);
				res.errmsg = NULL;
			}

/* since this gets forwarded to getaddrinfo we need to convert it back to a
 * decimal string in order for it to double as a 'service' reference */
			struct anet_options tmpcfg = *arg;
			tmpcfg.host = host;
			char buf[sizeof("65536")];
			snprintf(buf, sizeof(buf), "%"PRIu16, port);
			tmpcfg.port = buf;

			res = anet_cl_setup(&tmpcfg);
			free(host);
		}
		return res;
	}

	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	struct addrinfo* addr = NULL;

	int ec = getaddrinfo(arg->host, arg->port, &hints, &addr);
	if (ec){
		char buf[64];
		snprintf(buf, sizeof(buf), "couldn't resolve: %s", gai_strerror(ec));
		res.errmsg = strdup(buf);

		return res;
	}

/* related to keystore - if we get multiple options for one key and don't
 * have a provided host, port, enumerate those before failing */
	res.fd = anet_clfd(addr);
	if (-1 == res.fd){
		char buf[64];
		snprintf(buf, sizeof(buf), "couldn't connect to %s:%s", arg->host, arg->port);
		res.errmsg = strdup(buf);

		freeaddrinfo(addr);
		return res;
	}

/* at this stage we have a valid connection, time to build the state machine */
	res.state = a12_client(arg->opts);

/* repeat until we fail or get authenticated */
	do {
		uint8_t* buf;
		size_t out = a12_flush(res.state, &buf, 0);
		while (out){
			ssize_t nw = write(res.fd, buf, out);
			if (nw == -1){
				if (errno == EAGAIN || errno == EINTR)
					continue;

				char buf[64];
				snprintf(buf, sizeof(buf), "write(%d) during authentication", errno);
				res.errmsg = strdup(buf);

				goto fail;
			}
			else {
				out -= nw;
				buf += nw;
			}
		}

		char inbuf[4096];
		ssize_t nr = read(res.fd, inbuf, 4096);
		if (nr > 0){
			a12_unpack(res.state, (uint8_t*)inbuf, nr, NULL, NULL);
		}
		else if (nr == 0 || (errno != EAGAIN && errno != EINTR)){
			char buf[64];
			snprintf(buf, sizeof(buf), "read(%d) during authentication", errno);
			res.errmsg = strdup(buf);

			goto fail;
		}

	} while (a12_poll(res.state) > 0 && !a12_auth_state(res.state));

	return res;

fail:
	if (-1 != res.fd)
		shutdown(res.fd, SHUT_RDWR);

	res.fd = -1;
	a12_free(res.state);
	res.state = NULL;
	return res;
}

bool anet_listen(struct anet_options* args, char** errdst,
	void (*dispatch)(struct a12_state* S, int fd, void* tag), void* tag)
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	if (errdst)
		*errdst = NULL;

/* normal address setup foreplay */
	struct addrinfo* addr = NULL;
	struct addrinfo hints = {
		.ai_flags = AI_PASSIVE
	};
	int ec = getaddrinfo(args->host, args->port, &hints, &addr);
	if (ec){
		if (errdst)
			asprintf(errdst, "couldn't resolve address: %s\n", gai_strerror(ec));
		return false;
	}

	char hostaddr[NI_MAXHOST];
	char hostport[NI_MAXSERV];
	ec = getnameinfo(addr->ai_addr, addr->ai_addrlen,
		hostaddr, sizeof(hostaddr), hostport, sizeof(hostport),
		NI_NUMERICSERV | NI_NUMERICHOST
	);

	if (ec){
		if (errdst)
			asprintf(errdst, "couldn't resolve address: %s\n", gai_strerror(ec));
		freeaddrinfo(addr);
		return false;
	}

/* bind / listen */
	int sockin_fd = socket(addr->ai_family, SOCK_STREAM, 0);
	if (-1 == sockin_fd){
		if (errdst)
			asprintf(errdst, "couldn't create socket: %s\n", strerror(ec));
		freeaddrinfo(addr);
		return false;
	}

	int optval = 1;
	setsockopt(sockin_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	setsockopt(sockin_fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
	setsockopt(sockin_fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

	ec = bind(sockin_fd, addr->ai_addr, addr->ai_addrlen);
	if (ec){
		if (errdst)
			asprintf(errdst,
				"error binding (%s:%s): %s\n", hostaddr, hostport, strerror(errno));
		freeaddrinfo(addr);
		close(sockin_fd);
		return false;
	}

	ec = listen(sockin_fd, 5);
	if (ec){
		if (errdst)
			asprintf(errdst,
				"couldn't listen (%s:%s): %s\n", hostaddr, hostport, strerror(errno));
		close(sockin_fd);
		freeaddrinfo(addr);
	}

/* build state machine, accept and dispatch */
	for(;;){
		struct sockaddr_storage in_addr;
		socklen_t addrlen = sizeof(addr);

		int infd = accept(sockin_fd, (struct sockaddr*) &in_addr, &addrlen);
		struct a12_state* ast = a12_server(args->opts);
		if (!ast){
			if (errdst)
				asprintf(errdst, "Couldn't allocate client state machine\n");
			close(infd);
			return false;
		}

		dispatch(ast, infd, tag);
	}
}
