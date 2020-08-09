/* Start, stop, and act on a single HTTP server
**
** Copyright (C) 1995-2015  Jef Poskanzer <jef@mail.acme.com>
** Copyright (C) 2016-2018  Joachim Nilsson <troglobit@gmail.com>
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNERS OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
** THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <config.h>
#include <stdio.h>
#include <stdint.h>
#include <syslog.h>
#include <sys/stat.h>

#include "fdwatch.h"
#include "libhttpd.h"
#include "merecat.h"
#include "srv.h"
#include "ssl.h"

extern int handle_newconnect(struct httpd *hs, struct timeval *tv, int fd);

static void lookup_hostname(char *hostname, uint16_t port,
			    sockaddr_t *sa4, size_t sa4_len, int *gotv4,
			    sockaddr_t *sa6, size_t sa6_len, int *gotv6)
{
#ifdef USE_IPV6
	struct addrinfo hints;
	char service[10];
	int gaierr;
	struct addrinfo *ai;
	struct addrinfo *ptr;
	struct addrinfo *aiv6;
	struct addrinfo *aiv4;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%d", port);
	if ((gaierr = getaddrinfo(hostname, service, &hints, &ai)) != 0) {
		syslog(LOG_CRIT, "getaddrinfo %s: %s", hostname, gai_strerror(gaierr));
		exit(1);
	}

	/* Find the first IPv6 and IPv4 entries. */
	aiv6 = NULL;
	aiv4 = NULL;
	for (ptr = ai; ptr; ptr = ptr->ai_next) {
		switch (ptr->ai_family) {
		case AF_INET6:
			if (!aiv6)
				aiv6 = ptr;
			break;

		case AF_INET:
			if (!aiv4)
				aiv4 = ptr;
			break;
		}
	}

	if (!aiv6) {
		*gotv6 = 0;
	} else {
		if (sa6_len < aiv6->ai_addrlen) {
			syslog(LOG_CRIT, "%s - sockaddr too small (%lu < %lu)",
			       hostname, (unsigned long)sa6_len, (unsigned long)aiv6->ai_addrlen);
			exit(1);
		}
		memset(sa6, 0, sa6_len);
		memmove(sa6, aiv6->ai_addr, aiv6->ai_addrlen);
		*gotv6 = 1;
	}

#ifdef __linux__
	/*
	 * On Linux listening to IN6ADDR_ANY_INIT means also listening
	 * to INADDR_ANY, so for this special case we do not need to
	 * try to bind() to both.  In fact, it will cause an error.
	 */
	if (!aiv4 || (aiv6 && !hostname))
#else
	if (!aiv4)
#endif
		*gotv4 = 0;
	else {
		if (sa4_len < aiv4->ai_addrlen) {
			syslog(LOG_CRIT, "%s - sockaddr too small (%lu < %lu)",
			       hostname, (unsigned long)sa4_len, (unsigned long)aiv4->ai_addrlen);
			exit(1);
		}
		memset(sa4, 0, sa4_len);
		memmove(sa4, aiv4->ai_addr, aiv4->ai_addrlen);
		*gotv4 = 1;
	}

	freeaddrinfo(ai);

#else /* USE_IPV6 */

	struct hostent *he;

	*gotv6 = 0;

	memset(sa4, 0, sa4_len);
	sa4->sa.sa_family = AF_INET;
	if (!hostname) {
		sa4->sa_in.sin_addr.s_addr = htonl(INADDR_ANY);
	} else {
		sa4->sa_in.sin_addr.s_addr = inet_addr(hostname);
		if ((int)sa4->sa_in.sin_addr.s_addr == -1) {
			he = gethostbyname(hostname);
			if (!he) {
#ifdef HAVE_HSTRERROR
				syslog(LOG_CRIT, "gethostbyname %s: %s", hostname, hstrerror(h_errno));
#else
				syslog(LOG_CRIT, "gethostbyname %s failed", hostname);
#endif
				exit(1);
			}
			if (he->h_addrtype != AF_INET) {
				syslog(LOG_CRIT, "%s - non-IP network address", hostname);
				exit(1);
			}
			memmove(&sa4->sa_in.sin_addr.s_addr, he->h_addr, he->h_length);
		}
	}
	sa4->sa_in.sin_port = htons(port);
	*gotv4 = 1;

#endif /* USE_IPV6 */
}

struct httpd *srv_init(struct srv *srv)
{
	struct httpd *hs;
	sockaddr_t sa4;
	sockaddr_t sa6;
	size_t i;
	void *ctx = NULL;
	int gotv4, gotv6;

	syslog(LOG_DEBUG, "Initializing server %s: port %d, ssl %s, path %s",
	       srv->title, srv->port, srv->ssl ? "on" : "off", srv->path);

	/* Resolve default port */
	if (!srv->port)
		srv->port = srv->ssl ? DEFAULT_HTTPS_PORT : DEFAULT_HTTP_PORT;

	/* Look up hostname now, in case we chroot(). */
	lookup_hostname(srv->host, srv->port, &sa4, sizeof(sa4), &gotv4, &sa6, sizeof(sa6), &gotv6);
	if (!(gotv4 || gotv6)) {
		syslog(LOG_ERR, "cannot find any valid address");
		exit(1);
	}

	/* Initialize SSL library and load cert files before we chroot */
	if (srv->ssl) {
		ctx = httpd_ssl_init(srv->certfile, srv->keyfile, srv->dhfile, srv->ssl_proto, srv->ciphers);
		if (!ctx) {
			httpd_ssl_log_errors();
			exit(1);
		}
	}

	/* Initialize the HTTP layer.  Got to do this before giving up root,
	** so that we can bind to a privileged port.
	*/
	hs = httpd_init(hostname, srv->port, ctx, charset, max_age, srv->path,
			0, no_symlink_check, do_vhost, do_global_passwd,
			url_pattern, local_pattern,
			no_empty_referers, do_list_dotfiles);
	if (!hs)
		goto err;

	if (httpd_cgi_init(hs, cgi_enabled, cgi_pattern, cgi_limit))
		goto release;

	for (i = 0; i < NELEMS(srv->redirect); i++)
		httpd_redirect_add(hs, srv->redirect[i].code,
				   srv->redirect[i].pattern,
				   srv->redirect[i].location);

	for (i = 0; i < NELEMS(srv->location); i++)
		httpd_location_add(hs, srv->location[i].pattern, srv->location[i].path);

	if (httpd_listen(hs, gotv4 ? &sa4 : NULL, gotv6 ? &sa6 : NULL))
		goto err;

	hs->logfile = srv->logfile;
	return hs;
release:
	srv_exit(hs);
err:
	syslog(LOG_CRIT, "Failed initializing server %s", srv->title);
	return NULL;
}

void srv_start(struct httpd *hs)
{
	if (hs->listen4_fd != -1)
		fdwatch_add_fd(hs->listen4_fd, NULL, FDW_READ);
	if (hs->listen6_fd != -1)
		fdwatch_add_fd(hs->listen6_fd, NULL, FDW_READ);
}

void srv_stop(struct httpd *hs)
{
	if (hs->listen4_fd != -1)
		fdwatch_del_fd(hs->listen4_fd);
	if (hs->listen6_fd != -1)
		fdwatch_del_fd(hs->listen6_fd);
	httpd_unlisten(hs);
}

int srv_connect(struct httpd *hs, struct timeval *tv)
{
	if (!hs)
		return 0;

	/* Is it a new connection? */
	if (hs->listen6_fd != -1 && fdwatch_check_fd(hs->listen6_fd)) {
		if (handle_newconnect(hs, tv, hs->listen6_fd))
			return 1;
	}

	if (hs->listen4_fd != -1 && fdwatch_check_fd(hs->listen4_fd)) {
		if (handle_newconnect(hs, tv, hs->listen4_fd))
			return 1;
	}

	return 0;
}

void srv_exit(struct httpd *hs)
{
	srv_stop(hs);
	httpd_exit(hs);
}
