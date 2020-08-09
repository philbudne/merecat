/* libConfuse based merecat.conf parser
**
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

#include <errno.h>
#include <stdio.h>
#include <syslog.h>
#include <sys/param.h>
#include <confuse.h>

#ifdef HAVE_ZLIB_H
#include <zlib.h>
#endif

#include "conf.h"
#include "merecat.h"

static cfg_t *cfg = NULL;

static void conf_errfunc(cfg_t *cfg, const char *format, va_list args)
{
	char fmt[80];

	if (cfg && cfg->filename && cfg->line)
		snprintf(fmt, sizeof(fmt), "%s:%d: %s", cfg->filename, cfg->line, format);
	else if (cfg && cfg->filename)
		snprintf(fmt, sizeof(fmt), "%s: %s", cfg->filename, format);
	else
		snprintf(fmt, sizeof(fmt), "%s", format);

	vsyslog(LOG_ERR, fmt, args);
}

static void conf_cgi(cfg_t *cfg)
{
	if (!cfg)
		return;

	cgi_pattern = (char *)cfg_title(cfg);
	cgi_enabled = cfg_getbool(cfg, "enabled");
	cgi_limit   = cfg_getint(cfg, "limit");
}

static void conf_php(cfg_t *cfg)
{
	if (!cfg || !cfg_getbool(cfg, "enabled")) {
	err:
		php_pattern = NULL;
		php_cgi     = NULL;
		return;
	}

	php_cgi     = cfg_getstr(cfg, "cgi-path");
	php_pattern = (char *)cfg_title(cfg);
	if (!php_pattern || !php_cgi || access(php_cgi, X_OK)) {
		syslog(LOG_WARNING, "Invalid PHP settings, check your php-cgi path and pattern!");
		goto err;
	}
}

static void conf_ssi(cfg_t *cfg)
{
	if (!cfg || !cfg_getbool(cfg, "enabled")) {
	err:
		ssi_cgi = NULL;
		ssi_pattern = NULL;
		return;
	}

	ssi_cgi = cfg_getstr(cfg, "cgi-path");
	ssi_silent = cfg_getbool(cfg, "silent");
	ssi_pattern = (char *)cfg_title(cfg);
	if (!ssi_pattern || !ssi_cgi || access(ssi_cgi, X_OK)) {
		syslog(LOG_WARNING, "Invalid SSI settings, check path and pattern!");
		goto err;
	}
}

static void conf_redirect(struct srv *srv, cfg_t *cfg)
{
	size_t i;

	for (i = 0; i < cfg_size(cfg, "redirect") && i < NELEMS(srv->redirect); i++) {
		cfg_t *red;

		red = cfg_getnsec(cfg, "redirect", i);
		if (!red)
			return;

		srv->redirect[i].pattern  = (char *)cfg_title(red);
		srv->redirect[i].code     = cfg_getint(red, "code");
		srv->redirect[i].location = cfg_getstr(red, "location");
	}
}

static void conf_srv_location(struct srv *srv, cfg_t *cfg)
{
	size_t i;

	for (i = 0; i < cfg_size(cfg, "location") && i < NELEMS(srv->location); i++) {
		cfg_t *loc;

		loc = cfg_getnsec(cfg, "location", i);
		if (!loc)
			return;

		srv->location[i].pattern = (char *)cfg_title(loc);
		srv->location[i].path    = cfg_getstr(loc, "path");
	}
}

static void conf_ssl(struct srv *srv, cfg_t *cfg)
{
	cfg_t *ssl;

	ssl = cfg_getnsec(cfg, "ssl", 0);
	if (!ssl) {
		srv->ssl = 0;
		return;
	}

#ifndef ENABLE_SSL
	syslog(LOG_ERR, "%s is not built with HTTPS support", PACKAGE_NAME);
#else
	srv->ssl       = 1;
	srv->ssl_proto = cfg_getstr(ssl, "protocol");
	srv->ciphers   = cfg_getstr(ssl, "ciphers");
	srv->certfile  = cfg_getstr(ssl, "certfile");
	srv->keyfile   = cfg_getstr(ssl, "keyfile");
	srv->dhfile    = cfg_getstr(ssl, "dhfile"); /* Optional */
	if (!srv->certfile || !srv->keyfile)
		syslog(LOG_ERR, "Missing SSL certificate file(s)");
#endif
}

int conf_srv(struct srv arr[], size_t len)
{
	size_t i;

	if (!cfg) {
		arr[0].title = "default";
		arr[0].host  = hostname;
		arr[0].port  = port;
		arr[0].path  = path;
		arr[0].ssl   = 0;

		return 1;
	}

	if (cfg_size(cfg, "server") == 0) {
		arr[0].title = "default";
		arr[0].host  = cfg_getstr(cfg, "hostname");
		arr[0].port  = cfg_getint(cfg, "port");
		arr[0].path  = path;

		conf_ssl(&arr[0], cfg);

		return 1;
	}

	for (i = 0; i < cfg_size(cfg, "server") && i < len; i++) {
		cfg_t *srv;

		srv = cfg_getnsec(cfg, "server", i);
		if (!srv)
			return -1;

		arr[i].title = (char *)cfg_title(srv);
		arr[i].host  = cfg_getstr(srv, "hostname");
		arr[i].port  = cfg_getint(srv, "port");
		arr[i].path  = cfg_getstr(srv, "path");
		arr[i].logfile = cfg_getstr(srv, "logfile"); /* optional */

		conf_ssl(&arr[i], srv);
		conf_redirect(&arr[i], srv);
		conf_srv_location(&arr[i], srv);
	}

	return (int)i;
}

static int read_config(char *fn)
{
	cfg_opt_t location_opts[] = {
		CFG_STR ("path", NULL, CFGF_NONE),
		CFG_END ()
	};
	cfg_opt_t redirect_opts[] = {
		CFG_STR ("location", NULL, CFGF_NONE),
		CFG_INT ("code", 301, CFGF_NONE),
		CFG_END ()
	};
	cfg_opt_t cgi_opts[] = {
		CFG_BOOL("enabled", 0, CFGF_NONE),
		CFG_INT ("limit", cgi_limit, CFGF_NONE),
		CFG_END ()
	};
	cfg_opt_t php_opts[] = {
		CFG_BOOL("enabled", 0, CFGF_NONE),
		CFG_STR ("pattern", "**.php", CFGF_NONE),
		CFG_STR ("cgi-path", "/usr/bin/php-cgi", CFGF_NONE),
		CFG_END ()
	};
	cfg_opt_t ssi_opts[] = {
		CFG_BOOL("enabled", 0, CFGF_NONE),
		CFG_BOOL("silent", 0, CFGF_NONE),
		CFG_STR ("pattern", "**.shtml", CFGF_NONE),
		CFG_STR ("cgi-path", "cgi-bin/ssi", CFGF_NONE),
		CFG_END ()
	};
	cfg_opt_t ssl_opts[] = {
		CFG_STR ("protocol", SSL_DEFAULT_PROTO, CFGF_NONE),
		CFG_STR ("ciphers", SSL_DEFAULT_CIPHERS, CFGF_NONE),
		CFG_STR ("certfile", NULL, CFGF_NONE),
		CFG_STR ("keyfile", NULL, CFGF_NONE),
		CFG_STR ("dhfile", NULL, CFGF_NONE),
		CFG_END ()
	};
	cfg_opt_t server_opts[] = {
		CFG_STR ("hostname", hostname, CFGF_NONE),
		CFG_INT ("port",     port, CFGF_NONE),
		CFG_STR ("path",     path, CFGF_NONE),
		CFG_SEC ("location", location_opts, CFGF_MULTI | CFGF_TITLE),
		CFG_SEC ("ssl",      ssl_opts, CFGF_MULTI),
		CFG_SEC ("redirect", redirect_opts, CFGF_MULTI | CFGF_TITLE),
		CFG_STR ("logfile",  NULL, CFGF_NONE),
		CFG_END ()
	};
	cfg_opt_t opts[] = {
		CFG_INT ("port", port, CFGF_NONE),
		CFG_BOOL("chroot", do_chroot, CFGF_NONE),
		CFG_INT ("compression-level", compression_level, CFGF_NONE),
		CFG_STR ("directory", dir, CFGF_NONE),
		CFG_STR ("data-directory", data_dir, CFGF_NONE),
		CFG_BOOL("global-passwd", do_global_passwd, CFGF_NONE),
		CFG_BOOL("check-symlinks", !no_symlink_check, CFGF_NONE),
		CFG_BOOL("check-referer", cfg_false, CFGF_NONE),
		CFG_STR ("charset", charset, CFGF_NONE),
		CFG_BOOL("list-dotfiles", cfg_false, CFGF_NONE),
		CFG_STR ("local-pattern", NULL, CFGF_NONE),
		CFG_STR ("url-pattern", NULL, CFGF_NONE),
		CFG_INT ("max-age", 0, CFGF_NONE),
		CFG_STR ("username", user, CFGF_NONE),
		CFG_STR ("hostname", hostname, CFGF_NONE),
		CFG_BOOL("virtual-host", do_vhost, CFGF_NONE),
		CFG_STR ("user-agent-deny", useragent_deny, CFGF_NONE),
		CFG_SEC ("cgi", cgi_opts, CFGF_MULTI | CFGF_TITLE),
		CFG_SEC ("php", php_opts, CFGF_MULTI | CFGF_TITLE),
		CFG_SEC ("ssi", ssi_opts, CFGF_MULTI | CFGF_TITLE),
		CFG_SEC ("ssl", ssl_opts, CFGF_MULTI),
		CFG_SEC ("server", server_opts, CFGF_MULTI | CFGF_TITLE),
		CFG_END ()
	};
	int rc = 0;

	cfg = cfg_init(opts, CFGF_NONE);
	if (!cfg) {
		syslog(LOG_ERR, "Failed initializing configuration file parser: %s", strerror(errno));
		return 1;
	}

	/* Custom logging, rather than default Confuse stderr logging */
	cfg_set_error_function(cfg, conf_errfunc);

	rc = cfg_parse(cfg, fn);
	switch (rc) {
	case CFG_FILE_ERROR:
		syslog(LOG_ERR, "Cannot read configuration file %s", fn);
		goto error;

	case CFG_PARSE_ERROR:
		syslog(LOG_ERR, "Parse error in %s", fn);
		cfg_free(cfg);
		exit(1);

	case CFG_SUCCESS:
		break;
	}

	port = cfg_getint(cfg, "port");
	do_chroot = cfg_getbool(cfg, "chroot");
	if (do_chroot)
		no_symlink_check = 1;
	dir = cfg_getstr(cfg, "directory");
	data_dir = cfg_getstr(cfg, "data-directory");

	if (cfg_getbool(cfg, "check-symlinks"))
		no_symlink_check = 0;

	user = cfg_getstr(cfg, "username");
	url_pattern = cfg_getstr(cfg, "url-pattern");
	local_pattern = cfg_getstr(cfg, "local-pattern");
	useragent_deny = cfg_getstr(cfg, "user-agent-deny");

	no_empty_referers = cfg_getbool(cfg, "check-referer");
	do_list_dotfiles = cfg_getbool(cfg, "list-dotfiles");

	hostname = cfg_getstr(cfg, "hostname");
	do_vhost = cfg_getbool(cfg, "virtual-host");
	do_global_passwd = cfg_getbool(cfg, "global-passwd");

	charset = cfg_getstr(cfg, "charset");
	max_age = cfg_getint(cfg, "max-age");

#ifdef HAVE_ZLIB_H
	compression_level = cfg_getint(cfg, "compression-level");
	if (compression_level < Z_DEFAULT_COMPRESSION)
		compression_level = Z_DEFAULT_COMPRESSION;
	if (compression_level > Z_BEST_COMPRESSION)
		compression_level = Z_BEST_COMPRESSION;
#endif

	conf_cgi(cfg_getnsec(cfg, "cgi", 0));
	conf_php(cfg_getnsec(cfg, "php", 0));
	conf_ssi(cfg_getnsec(cfg, "ssi", 0));

	return 0;
error:
	cfg_free(cfg);
	cfg = NULL;

	return 1;
}

int conf_init(char *file)
{
	char path[MAXPATHLEN + 1];

	if (!file) {
		snprintf(path, sizeof(path), "%s/%s.conf", CONFDIR, ident);
		file = path;

		/*
		 * If default .conf doesn't exist, fail silent.
		 * We must support running stand-alone as well.
		 */
		if (access(file, F_OK))
			return 0;
	} else {
		/*
		 * Support stand-alone also if `-f none`
		 */
		if (!strcmp(file, "none"))
			return 0;

		/*
		 * If `-f foo.conf` doesn't exist, we must bail, the
		 * user expects their settings from the .conf not any
		 * built-in defaults.
		 */
		if (access(file, F_OK)) {
			syslog(LOG_ERR, "%s: %s: %s", prognm, file, strerror(errno));
			return 1;
		}
	}

	if (read_config(file)) {
		fprintf(stderr, "%s: Failed reading config file '%s'\n", prognm, file);
		return 1;
	}

	return 0;
}

void conf_exit(void)
{
	cfg_free(cfg);
}
