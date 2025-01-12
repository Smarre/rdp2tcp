/**
 * @file tunnel.c
 * rdp2tcp tunnels management
 */
/*
 * This file is part of rdp2tcp
 *
 * Copyright (C) 2010-2011, Nicolas Collignon
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "rdp2tcp.h"
#include "r2twin.h"
#include "print.h"

#include <stdio.h>

extern const char *r2t_errors[R2TERR_MAX];

/** global tunnels double-linked list */
LIST_HEAD_INIT(all_tunnels);

/** lookup rdp2tcp tunnel
 * @param[in] id rdp2tcp tunnel ID
 * @return NULL if tunnel is not found */
tunnel_t *tunnel_lookup(unsigned char id)
{
	tunnel_t *tun;

	//trace_tun("id=0x%02x", id);
	list_for_each(tun, &all_tunnels) {
		if (tun->id == id)
			return tun;
	}

	return NULL;
}

static unsigned char wsa_to_r2t_error(int err)
{
	switch (err) {
		case WSAEACCES: return R2TERR_FORBIDDEN;
		case WSAECONNREFUSED: return R2TERR_CONNREFUSED;
		case WSAEADDRNOTAVAIL: return R2TERR_NOTAVAIL;
		case WSAHOST_NOT_FOUND: return R2TERR_RESOLVE;
	}

	return R2TERR_GENERIC;
}

/**
 * generate a unused tunnel ID
 * @return 0xff on error (all tunnel ID are used)
	// in most cases tunnel IDs are generated by the client
	// this is the single case where it is generated by the server
 */
static unsigned char tunnel_generate_id(void)
{
	unsigned char tid;
	static unsigned char last_tid = 0xff;

	for (tid=last_tid+1; tid!=last_tid; ++tid) {
		if (!tunnel_lookup(tid)) {
			last_tid = tid;
			return tid;
		}
	}

	return 0xff;
}

static unsigned int netaddr_to_connans(
			const netaddr_t *addr,
			r2tmsg_connans_t *msg)
{
	unsigned int msg_len;

	memset(msg, 0, sizeof(*msg));
	msg->err = R2TERR_SUCCESS;

	if (netaddr_af(addr) == AF_INET) {
		msg->af   = TUNAF_IPV4;
		msg->port = addr->ip4.sin_port;
		memcpy(&msg->addr, &addr->ip4.sin_addr, 4);
		msg_len = 8;
	} else {
		msg->af   = TUNAF_IPV6;
		msg->port = addr->ip6.sin6_port;
		memcpy(&msg->addr, &addr->ip6.sin6_addr, 16);
		msg_len = 20;
	}

	return msg_len;
}

static int tunnel_socksend_event(tunnel_t *tun)
{
	int ret;
	unsigned int w;

	assert(valid_tunnel(tun));
	trace_tun("id=0x%02x, used=%u", tun->id, iobuf_datalen(&tun->wio.buf));

	ret = net_write(&tun->sock, &tun->wio.buf, NULL, 0, &w);
	if (ret < 0)
		return error("%s", net_error(NETERR_SEND, ret));

	if (w > 0)
		print_xfer("tcp", 'w', w);

	return 0;
}

static int tunnel_connect_event(tunnel_t *tun, int err)
{
	unsigned int ans_len;
	r2tmsg_connans_t ans;
	char host[NETADDRSTR_MAXSIZE];

	trace_tun("id=0x%02x, err=%i", tun->id, err);

	memset(&ans, 0, sizeof(ans));
	ans.err = R2TERR_GENERIC;
	ans_len = 1;

	if (!err) {
		tun->connected = 1;
		info(0, "tunnel 0x%02x connected to %s", tun->id,
            netaddr_print(&tun->addr, host));

		if (!net_update_watch(&tun->sock, &tun->wio.buf)) {
			if (iobuf_datalen(&tun->wio.buf) && (tunnel_socksend_event(tun) < 0))
				err = 1;
			if (!err)
				ans_len = netaddr_to_connans(&tun->addr, &ans);
		}

	} else {
		ans.err = wsa_to_r2t_error(err);
	}

	if (ans.err != R2TERR_SUCCESS) {
		error("failed to connect tunnel 0x%02x (%i %s)", tun->id,
				err, r2t_errors[ans.err]);
	}

	if (channel_write(R2TCMD_CONN, tun->id, &ans.err, ans_len) >= 0) {
		if (ans.err == R2TERR_SUCCESS)
			return 0;
	}

	return -1;
}


static int host_connect(
					tunnel_t *tun,
					int pref_af,
					const char *host,
					unsigned short port)
{
	int ret, err;
	unsigned char msg;

	ret = net_client(pref_af, host, port, &tun->sock, &tun->addr, &err);
	debug(0, "net_client(%s, %hu) -> %i / %i", host, port, ret, err);

	if (ret >= 0) {
		info(0, "connect%s to %s:%hu", (ret > 0 ? "ing" : "ed"),
			host, port);

		if (!event_add_tunnel(tun->sock.evt, tun->id)) {
			iobuf_init2(&tun->rio.buf, &tun->wio.buf, "tcp");
			if (!ret) {
				ret = tunnel_connect_event(tun, 0);
			} else {
				//WSAEventSelect(tun->sock.fd, tun->sock.evt, FD_CONNECT|FD_CLOSE);
			}
			return ret;
		}
	}

	msg = wsa_to_r2t_error(err);
	channel_write(R2TCMD_CONN, tun->id, &msg, 1);
	if (ret >= 0)
		net_close(&tun->sock);

	return -1;
}

static int host_bind(
		tunnel_t *tun,
		int pref_af,
		const char *host,
		unsigned short port)
{
	int ret, err;
	unsigned int ans_len;
	r2tmsg_connans_t ans;

	memset(&ans, 0, sizeof(ans));
	ans_len = 1;

	ret = net_server(pref_af, host, port, &tun->sock, &tun->addr, &err);
	debug(0, "bind %s:%hu ... %i/%i", host, port, ret, err);
	if (!ret) {
		info(0, "listening on %s:%hu", host, port);
		ans_len = netaddr_to_connans(&tun->addr, &ans);
		ans.err = 0;
		if (event_add_tunnel(tun->sock.evt, tun->id)) {
			ans.err = R2TERR_GENERIC;
			net_close(&tun->sock);
			ret = -1;
		}

	} else {
		ans.err = wsa_to_r2t_error(err);
		error("failed to bind %s:%hu (%i %s)", host, port, err, r2t_errors[ans.err]);
	}

	if (channel_write(R2TCMD_BIND, tun->id, &ans.err, ans_len) >= 0) {
		if (!ans.err) {
			tun->connected = 1;
			tun->server = 1;
			return 0;
		}
	}

	if (!ret) {
		event_del_tunnel(tun->id);
		net_close(&tun->sock);
	}

	return -1;
}


static tunnel_t *tunnel_alloc(unsigned char id)
{
	tunnel_t *tun;

	tun = calloc(1, sizeof(*tun));
	if (tun) {
		tun->id = id;
	} else {
		error("failed to allocate tunnel");
	}

	return tun;
}

/**
 * create rdp2tcp tunnel
 * @param[in] id rdp2tcp tunnel ID
 * @param[in] pref_af preferred address family
 * @param[in] host tunnel hostname or command line
 * @param[in] port tcp tunnel port or 0 for process tunnel
 * @param[in] bind_socket 1 for reverse connect tunnel
 */
void tunnel_create(
			unsigned char id,
			int pref_af,
			const char *host,
			unsigned short port,
			int bind_socket)
{
	tunnel_t *tun;
	int ret;

	assert(host && *host);
	trace_tun("id=0x%02x, pref_af=%i, host=%s, port=%hu", id, pref_af, host, port);

	tun = tunnel_alloc(id);
	if (!tun)
		return;

	if (port > 0) {
		// tcp tunnel
		if (!bind_socket)
			ret = host_connect(tun, pref_af, host, port);
		else
			ret = host_bind(tun, pref_af, host, port);
	} else {
		// process stdin/out tunnel
		ret = process_start(tun, host);
	}

	if (ret >= 0) {
		list_add_tail(&tun->list, &all_tunnels);
		debug(0, "tunnel 0x%02x created", id);

	} else {
		debug(0, "failed to create tunnel 0x%02x", id);
		free(tun);
	}
}

/** close rdp2tcp tunnel
 * @param[in] tun established tunnel */
void tunnel_close(tunnel_t *tun)
{
	assert(valid_tunnel(tun));
	trace_tun("id=0x%02x", tun->id);

	list_del(&tun->list);

	event_del_tunnel(tun->id);

	if (!tun->proc) {
		if (!tun->server)
			iobuf_kill2(&tun->rio.buf, &tun->wio.buf);
		net_close(&tun->sock);

	} else {
		CancelIo(tun->rfd);
		CancelIo(tun->wfd);
		process_stop(tun);
	}

	free(tun);
}

static int tunnel_sockrecv_event(tunnel_t *tun)
{
	int ret;
	unsigned int r;

	assert(valid_tunnel(tun));

	ret = net_read(&tun->sock, &tun->rio.buf, 0, &tun->rio.min_io_size, &r);
	trace_tun("id=0x%02x --> ret=%i, r=%u", tun->id, ret, r);
	if (ret < 0)
		return error("%s", net_error(NETERR_RECV, ret));

	if (r > 0) {
		print_xfer("tcp", 'r', r);
		if (channel_forward(tun) < 0)
			return error("failed to forward");

	//	if (net_update_watch(&tun->sock, &tun->wio.buf))
	//		return wsaerror("WSAEventSelect");
	}

	return 0;
}

static int on_read_completed(iobuf_t *ibuf, tunnel_t *tun)
{
	assert(valid_iobuf(ibuf) && valid_tunnel(tun));
	return channel_forward(tun);
}

static int tunnel_fdread_event(tunnel_t *tun)
{
	assert(valid_tunnel(tun));
	return aio_read(&tun->rio, tun->rfd, "tun",
							(aio_readcb_t)on_read_completed, tun);
}

static int tunnel_fdwrite_event(tunnel_t *tun)
{
	assert(valid_tunnel(tun));
	return aio_write(&tun->wio, tun->wfd, "tun");
}

static int tunnel_accept_event(tunnel_t *tun)
{
	tunnel_t *cli;
	sock_t cli_sock;
	int ret;
	unsigned char tid;
	unsigned int msg_len;
	netaddr_t addr;
	r2tmsg_rconnreq_t msg;
	char host[NETADDRSTR_MAXSIZE];

	assert(valid_tunnel(tun));
	trace_tun("id=0x%02x", tun->id);

	ret = net_accept(&tun->sock, &cli_sock, &addr);
	if (ret)
		return wsaerror("accept");

	tid = tunnel_generate_id();
	if (tid == 0xff) {
		error("failed to generate tunnel identifier");
		net_close(&cli_sock);
		return 0;
	}
	trace_tun("srvid=0x%02x cliid=0x%02x", tun->id, tid);

	info(0, "accepted %s on tunnel 0x%02x",
			netaddr_print(&addr, host), tun->id);

	cli = tunnel_alloc(tid);
	if (!cli) {
		net_close(&cli_sock);
		return 0; // soft error
	}

	if (event_add_tunnel(cli_sock.evt, tid)) {
		net_close(&cli_sock);
		free(cli);
		return 0; // soft error
	}
	cli->sock.fd   = cli_sock.fd;
	cli->sock.evt  = cli_sock.evt;
	cli->connected = 1;
	cli->id        = tid;
	iobuf_init2(&cli->rio.buf, &cli->wio.buf, "tcp");
	list_add_tail(&cli->list, &all_tunnels);

	msg_len = netaddr_to_connans(&addr, (r2tmsg_connans_t *)&msg);
	msg.rid = tid;

	if (channel_write(R2TCMD_RCONN, tun->id, &msg.rid, msg_len) < 0)
		tunnel_close(tun);

	return 0;
}

static int tunnel_close_event(tunnel_t *tun)
{
	assert(valid_tunnel(tun));

	channel_write(R2TCMD_CLOSE, tun->id, NULL, 0);
	tunnel_close(tun);

	return 0;
}

/** handle tunnel event
 * @param[in] tun tunnel associated with event
 * @param[in] h event handle
 * @return 0 on success
 */
int tunnel_event(tunnel_t *tun, HANDLE h)
{
	int ret, evt;
	WSANETWORKEVENTS events;

	assert(valid_tunnel(tun) && h);
	trace_tun("id=0x%02x %s h=%x", tun->id, tun->proc ? "proc" : "tcp", h);

	if (tun->proc) { // process tunnel

		if (h == tun->proc) { // process is dead
			info(0, "tunnel 0x%02x process has terminated", tun->id);
			return tunnel_close_event(tun);
		}
		if (h == tun->rio.io.hEvent) {
			ret = tunnel_fdread_event(tun);
		} else {
			assert(h == tun->wio.io.hEvent);
			ret = tunnel_fdwrite_event(tun);
		}

	} else { // socket tunnel

		ret = 0;
		events.lNetworkEvents = 0;

		if (!WSAEnumNetworkEvents(tun->sock.fd, tun->sock.evt, &events)) {

			evt = (int) events.lNetworkEvents;

			debug(1, "close=%i, conn=%i/%i, read=%i, write=%i, accept=%i",
					!!(evt & FD_CLOSE), !!(evt & FD_CONNECT), tun->connected,
					!!(evt & FD_READ), !!(evt & FD_WRITE),
					!!(evt & FD_ACCEPT));

			if (evt & FD_ACCEPT) {
				debug(0, "FD_ACCEPT");
				ret = tunnel_accept_event(tun);

			} else if (evt & FD_CONNECT) {
				debug(0, "FD_CONNECT");
				ret = tunnel_connect_event(tun, events.iErrorCode[FD_CONNECT_BIT]);
				if (!ret) {
					assert(tun->connected);
					ret = tunnel_socksend_event(tun);
					if (ret >= 0)
						ret = tunnel_sockrecv_event(tun);
				}

			} else if (evt & FD_WRITE) {
				debug(0, "FD_WRITE");
				ret = tunnel_socksend_event(tun);
			}

			if ((ret >= 0) && (evt & FD_READ)) {
				debug(0, "FD_READ");
				ret = tunnel_sockrecv_event(tun);
			}

			if (evt & FD_CLOSE) {
				debug(0, "FD_CLOSE");
				return tunnel_close_event(tun);
			}

		} else {
			if (WSAGetLastError() != ERROR_IO_PENDING)
				return wsaerror("WSAEnumNetworkEvents");
		}
	}

	if (ret < 0)
		tunnel_close(tun);

	return 0;
}

/** write data ti rdp2tcp tunnel
 * @param[in] tun established tunnel
 * @param[in] data data to write
 * @param[in] len buffer size
 * @return 0 on success */
int tunnel_write(tunnel_t *tun, const void *data, unsigned int len)
{
	unsigned int used;
	iobuf_t *obuf;

	assert(valid_tunnel(tun) && data && len);
	trace_tun("id=0x%02x, data=%p, len=%u, connected=%i",
				tun->id, data, len, tun->connected);

	obuf = &tun->wio.buf;
	trace_tun("====> %c %u %u", obuf->type, obuf->size, obuf->total);
	assert(valid_iobuf(obuf));

	used = iobuf_datalen(obuf);
	if (len > 0) {
		if (!iobuf_append(obuf, data, len))
			return error("failed to append %u bytes to tunnel buffer", len);
	}

	if ((used > 0) || !tun->connected)
		return 0;

	if (tun->proc)
		return tunnel_fdwrite_event(tun);

	if (net_update_watch(&tun->sock, &tun->wio.buf)) {
		return wsaerror("WSAEventSelect");
	}

	return tunnel_socksend_event(tun);
}

/** destroy all tunnels */
void tunnels_kill(void)
{
	tunnel_t *tun, *bak;

	trace_tun("");

	list_for_each_safe(tun, bak, &all_tunnels) {
		tunnel_close(tun);
	}
}

