/*-
 * Copyright (c) 2008-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtc.h"

#include "vsa.h"
#include "vss.h"
#include "vtcp.h"

struct client {
	unsigned		magic;
#define CLIENT_MAGIC		0x6242397c
	char			*name;
	struct vtclog		*vl;
	VTAILQ_ENTRY(client)	list;

	char			*spec;

	char			connect[256];

	char			*proxy_spec;
	int			proxy_version;

	unsigned		repeat;

	unsigned		running;
	pthread_t		tp;
};

static VTAILQ_HEAD(, client)	clients =
    VTAILQ_HEAD_INITIALIZER(clients);

/**********************************************************************
 * Send the proxy header
 */

static int __match_proto__(vss_resolved_f)
proxy_cb(void *priv, const struct suckaddr *sa)
{
	struct suckaddr **addr = priv;
	*addr = VSA_Clone(sa);
	return (1);
}

static void
client_proxy(struct vtclog *vl, int fd, int version, const char *spec)
{
	struct suckaddr *sac, *sas;
	const char *err;
	char *p, *p2;
	int error;

	p = strdup(spec);
	AN(p);
	p2 = strchr(p, ' ');
	AN(p2);
	*p2++ = '\0';

	error = VSS_resolver(p, NULL, proxy_cb, &sac, &err);
	if (err != NULL)
		vtc_fatal(vl, "Could not resolve client address: %s", err);
	assert(error == 1);
	error = VSS_resolver(p2, NULL, proxy_cb, &sas, &err);
	if (err != NULL)
		vtc_fatal(vl, "Could not resolve server address: %s", err);
	assert(error == 1);
	if (vtc_send_proxy(fd, version, sac, sas))
		vtc_fatal(vl, "Write failed: %s", strerror(errno));
	free(p);
	free(sac);
	free(sas);
}

/**********************************************************************
 * Client thread
 */

static void *
client_thread(void *priv)
{
	struct client *c;
	struct vtclog *vl;
	int fd;
	unsigned u;
	struct vsb *vsb;
	char *p;
	char mabuf[32], mpbuf[32];
	const char *err;

	CAST_OBJ_NOTNULL(c, priv, CLIENT_MAGIC);
	AN(*c->connect);

	vl = vtc_logopen(c->name);

	p = strdup(c->connect);
	AN(p);
	vsb = macro_expand(vl, p);
	AN(vsb);

	if (c->repeat == 0)
		c->repeat = 1;
	if (c->repeat != 1)
		vtc_log(vl, 2, "Started (%u iterations)", c->repeat);
	for (u = 0; u < c->repeat; u++) {
		vtc_log(vl, 3, "Connect to %s", VSB_data(vsb));
		fd = VTCP_open(VSB_data(vsb), NULL, 10., &err);
		if (fd < 0)
			vtc_fatal(c->vl, "Failed to open %s: %s",
			    VSB_data(vsb), err);
		assert(fd >= 0);
		/* VTCP_blocking does its own checks, trust it */
		(void)VTCP_blocking(fd);
		VTCP_myname(fd, mabuf, sizeof mabuf, mpbuf, sizeof mpbuf);
		vtc_log(vl, 3, "connected fd %d from %s %s to %s",
		    fd, mabuf, mpbuf, VSB_data(vsb));
		if (c->proxy_spec != NULL)
			client_proxy(vl, fd, c->proxy_version, c->proxy_spec);
		fd = http_process(vl, c->spec, fd, NULL);
		vtc_log(vl, 3, "closing fd %d", fd);
		VTCP_close(&fd);
	}
	vtc_log(vl, 2, "Ending");
	VSB_destroy(&vsb);
	free(p);
	return (NULL);
}

/**********************************************************************
 * Allocate and initialize a client
 */

static struct client *
client_new(const char *name)
{
	struct client *c;

	AN(name);
	ALLOC_OBJ(c, CLIENT_MAGIC);
	AN(c);
	REPLACE(c->name, name);
	c->vl = vtc_logopen(name);
	AN(c->vl);
	if (*c->name != 'c')
		vtc_fatal(c->vl, "Client name must start with 'c'");

	bprintf(c->connect, "%s", "${v1_sock}");
	VTAILQ_INSERT_TAIL(&clients, c, list);
	return (c);
}

/**********************************************************************
 * Clean up client
 */

static void
client_delete(struct client *c)
{

	CHECK_OBJ_NOTNULL(c, CLIENT_MAGIC);
	vtc_logclose(c->vl);
	free(c->spec);
	free(c->name);
	free(c->proxy_spec);
	/* XXX: MEMLEAK (?)*/
	FREE_OBJ(c);
}

/**********************************************************************
 * Start the client thread
 */

static void
client_start(struct client *c)
{

	CHECK_OBJ_NOTNULL(c, CLIENT_MAGIC);
	vtc_log(c->vl, 2, "Starting client");
	AZ(pthread_create(&c->tp, NULL, client_thread, c));
	c->running = 1;
}

/**********************************************************************
 * Wait for client thread to stop
 */

static void
client_wait(struct client *c)
{
	void *res;

	CHECK_OBJ_NOTNULL(c, CLIENT_MAGIC);
	vtc_log(c->vl, 2, "Waiting for client");
	AZ(pthread_join(c->tp, &res));
	if (res != NULL)
		vtc_fatal(c->vl, "Client returned \"%s\"", (char *)res);
	c->tp = 0;
	c->running = 0;
}

/**********************************************************************
 * Run the client thread
 */

static void
client_run(struct client *c)
{

	client_start(c);
	client_wait(c);
}


/**********************************************************************
 * Client command dispatch
 */

void
cmd_client(CMD_ARGS)
{
	struct client *c, *c2;

	(void)priv;
	(void)cmd;
	(void)vl;

	if (av == NULL) {
		/* Reset and free */
		VTAILQ_FOREACH_SAFE(c, &clients, list, c2) {
			VTAILQ_REMOVE(&clients, c, list);
			if (c->tp != 0)
				client_wait(c);
			client_delete(c);
		}
		return;
	}

	AZ(strcmp(av[0], "client"));
	av++;

	VTAILQ_FOREACH(c, &clients, list)
		if (!strcmp(c->name, av[0]))
			break;
	if (c == NULL)
		c = client_new(av[0]);
	av++;

	for (; *av != NULL; av++) {
		if (vtc_error)
			break;

		if (!strcmp(*av, "-wait")) {
			client_wait(c);
			continue;
		}

		/* Don't muck about with a running client */
		if (c->running)
			client_wait(c);

		if (!strcmp(*av, "-connect")) {
			bprintf(c->connect, "%s", av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-proxy1")) {
			REPLACE(c->proxy_spec, av[1]);
			c->proxy_version = 1;
			av++;
			continue;
		}
		if (!strcmp(*av, "-proxy2")) {
			REPLACE(c->proxy_spec, av[1]);
			c->proxy_version = 2;
			av++;
			continue;
		}
		if (!strcmp(*av, "-repeat")) {
			c->repeat = atoi(av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-start")) {
			client_start(c);
			continue;
		}
		if (!strcmp(*av, "-run")) {
			client_run(c);
			continue;
		}
		if (**av == '-')
			vtc_fatal(c->vl, "Unknown client argument: %s", *av);
		REPLACE(c->spec, *av);
	}
}
