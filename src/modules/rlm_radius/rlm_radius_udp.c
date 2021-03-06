/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_radius_udp.c
 * @brief RADIUS UDP transport
 *
 * @copyright 2017  Network RADIUS SARL
 */
RCSID("$Id$")

#include <freeradius-devel/io/application.h>
#include <freeradius-devel/udp.h>
#include <freeradius-devel/heap.h>
#include <freeradius-devel/connection.h>
#include <freeradius-devel/io/listen.h>
#include <freeradius-devel/rad_assert.h>

#include "rlm_radius.h"
#include "track.h"

/** Static configuration for the module.
 *
 */
typedef struct rlm_radius_udp_t {
	rlm_radius_t		*parent;		//!< rlm_radius instance
	CONF_SECTION		*config;

	fr_ipaddr_t		dst_ipaddr;		//!< IP of the home server
	fr_ipaddr_t		src_ipaddr;		//!< IP we open our socket on
	uint16_t		dst_port;		//!< port of the home server
	char const		*secret;		//!< shared secret

	char const		*interface;		//!< Interface to bind to.

	uint32_t		recv_buff;		//!< How big the kernel's receive buffer should be.
	uint32_t		send_buff;		//!< How big the kernel's send buffer should be.

	uint32_t		max_packet_size;	//!< maximum packet size

	bool			recv_buff_is_set;	//!< Whether we were provided with a recv_buf
	bool			send_buff_is_set;	//!< Whether we were provided with a send_buf
	bool			replicate;		//!< copied from parent->replicate
} rlm_radius_udp_t;


/** Per-thread configuration for the module.
 *
 *  This data structure holds the connections, etc. for this IO submodule.
 */
typedef struct rlm_radius_udp_thread_t {
	rlm_radius_udp_t	*inst;		//!< IO submodule instance
	fr_event_list_t		*el;		//!< event list

	bool			pending;	//!< are there pending requests?

	fr_heap_t		*queued;	//!< queued requests for some new connection

	fr_heap_t		*active;       	//!< active connections
	fr_dlist_t		full;      	//!< full connections
	fr_dlist_t		zombie;      	//!< zombie connections
	fr_dlist_t		opening;      	//!< opening connections
} rlm_radius_udp_thread_t;

typedef enum rlm_radius_udp_connection_state_t {
	CONN_UNUSED = 0,
	CONN_OPENING,				//!< trying to connect
	CONN_ACTIVE,				//!< available to send packets
	CONN_FULL,				//!< live, but can't send more packets
	CONN_ZOMBIE,				//!< has had a retransmit timeout
	CONN_STATUS_CHECKS,			//!< status-checks, nothing else
} rlm_radius_udp_connection_state_t;

typedef struct rlm_radius_udp_request_t rlm_radius_udp_request_t;

typedef struct rlm_radius_udp_connection_t {
	rlm_radius_udp_t const	*inst;		//!< our module instance
	rlm_radius_udp_thread_t *thread;       	//!< our thread-specific data
	fr_connection_t		*conn;		//!< Connection to our destination.
	char const     		*name;		//!< from IP PORT to IP PORT

	fr_dlist_t		entry;		//!< in the linked list of connections
	int			heap_id;	//!< for the active heap
	rlm_radius_udp_connection_state_t state; //!< state of the connection

	fr_event_timer_t const	*idle_ev;	//!< idle timeout event
	struct timeval		idle_timeout;	//!< when the idle timeout will fire

	struct timeval		mrs_time;	//!< most recent sent time which had a reply
	fr_event_timer_t const	*zombie_ev;	//!< zombie timeout
	struct timeval		zombie_start;	//!< when the zombie period started

	int			num_requests;	//!< number of packets we sent, NOT including Status-Server
	int			max_requests;	//!< maximum number of packets we can send

	bool			pending;	//!< are there packets pending?
	fr_heap_t		*queued;       	//!< list of packets queued for sending
	fr_dlist_t		sent;		//!< list of sent packets

	uint32_t		max_packet_size; //!< our max packet size. may be different from the parent...
	int			fd;		//!< file descriptor

	fr_ipaddr_t		dst_ipaddr;	//!< IP of the home server. stupid 'const' issues..
	uint16_t		dst_port;	//!< port of the home server
	fr_ipaddr_t		src_ipaddr;	//!< my source IP
	uint16_t	       	src_port;	//!< my source port

	uint8_t			*buffer;	//!< receive buffer
	size_t			buflen;		//!< receive buffer length

	rlm_radius_udp_request_t *status_u;    	//!< for Status-Server checks

	rlm_radius_id_t		*id;		//!< ID tracking
} rlm_radius_udp_connection_t;


/** Link a packet to a connection
 *
 */
struct rlm_radius_udp_request_t {
	fr_dlist_t		entry;		//!< in the connection list of packets
	int			heap_id;	//!< for the "to be sent" queue.

	int			code;		//!< packet code
	rlm_radius_udp_connection_t	*c;     //!< the connection
	rlm_radius_link_t	*link;		//!< more link stuff
	rlm_radius_request_t	*rr;		//!< the ID tracking, resend count, etc.
	uint8_t			*packet;	//!< packet we write to the network
	size_t			packet_len;	//!< length of the packet
};


static const CONF_PARSER module_config[] = {
	{ FR_CONF_OFFSET("ipaddr", FR_TYPE_COMBO_IP_ADDR, rlm_radius_udp_t, dst_ipaddr), },
	{ FR_CONF_OFFSET("ipv4addr", FR_TYPE_IPV4_ADDR, rlm_radius_udp_t, dst_ipaddr) },
	{ FR_CONF_OFFSET("ipv6addr", FR_TYPE_IPV6_ADDR, rlm_radius_udp_t, dst_ipaddr) },

	{ FR_CONF_OFFSET("port", FR_TYPE_UINT16, rlm_radius_udp_t, dst_port) },

	{ FR_CONF_OFFSET("secret", FR_TYPE_STRING | FR_TYPE_REQUIRED, rlm_radius_udp_t, secret) },

	{ FR_CONF_OFFSET("interface", FR_TYPE_STRING, rlm_radius_udp_t, interface) },

	{ FR_CONF_IS_SET_OFFSET("recv_buff", FR_TYPE_UINT32, rlm_radius_udp_t, recv_buff) },
	{ FR_CONF_IS_SET_OFFSET("send_buff", FR_TYPE_UINT32, rlm_radius_udp_t, send_buff) },

	{ FR_CONF_OFFSET("max_packet_size", FR_TYPE_UINT32, rlm_radius_udp_t, max_packet_size),
	  .dflt = "4096" },

	{ FR_CONF_OFFSET("src_ipaddr", FR_TYPE_COMBO_IP_ADDR, rlm_radius_udp_t, src_ipaddr) },
	{ FR_CONF_OFFSET("src_ipv4addr", FR_TYPE_IPV4_ADDR, rlm_radius_udp_t, src_ipaddr) },
	{ FR_CONF_OFFSET("src_ipv6addr", FR_TYPE_IPV6_ADDR, rlm_radius_udp_t, src_ipaddr) },

	CONF_PARSER_TERMINATOR
};

static void conn_error(UNUSED fr_event_list_t *el, UNUSED int fd, UNUSED int flags, int fd_errno, void *uctx);
static void conn_read(UNUSED fr_event_list_t *el, int fd, UNUSED int flags, void *uctx);
static void conn_writable(UNUSED fr_event_list_t *el, int fd, UNUSED int flags, void *uctx);
static void mod_clear_backlog(rlm_radius_udp_thread_t *t);

static int conn_cmp(void const *one, void const *two)
{
	rlm_radius_udp_connection_t const *a = talloc_get_type_abort(one, rlm_radius_udp_connection_t);
	rlm_radius_udp_connection_t const *b = talloc_get_type_abort(two, rlm_radius_udp_connection_t);

	if (timercmp(&a->mrs_time, &b->mrs_time, <)) return -1;
	if (timercmp(&a->mrs_time, &b->mrs_time, >)) return -1;

	if (a->id->num_free < b->id->num_free) return -1;
	if (a->id->num_free > b->id->num_free) return +1;

	return 0;
}


/** Compare two packets in the "to be sent" queue.
 *
 *  Status-Server packets are always sorted before other packets, by
 *  virtue of request->async->recv_time always being zero.
 */
static int queue_cmp(void const *one, void const *two)
{
	rlm_radius_udp_request_t const *a = one;
	rlm_radius_udp_request_t const *b = two;

	if (a->link->request->async->recv_time < b->link->request->async->recv_time) return -1;
	if (a->link->request->async->recv_time > b->link->request->async->recv_time) return +1;

	return 0;
}


/** Close a socket due to idle timeout
 *
 */
static void conn_idle_timeout(UNUSED fr_event_list_t *el, UNUSED struct timeval *now, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);

	DEBUG("%s TIMER - idle timeout for connection %s",
	      c->inst->parent->name, c->name);

	talloc_free(c);
}


/** The connection is idle, set up idle timeouts.
 *
 */
static void conn_idle(rlm_radius_udp_connection_t *c)
{
	struct timeval when;

	/*
	 *	We set idle (or not) depending on the conneciton
	 *	state.
	 */
	switch (c->state) {
	case CONN_UNUSED:
	case CONN_OPENING:
		rad_assert(0 == 1);
		return;

		/*
		 *	Still has packets: can't be idle.
		 */
	case CONN_FULL:
		rad_assert(c->num_requests > 0);
		if (c->idle_ev) (void) fr_event_timer_delete(c->thread->el, &c->idle_ev);
		return;

		/*
		 *	Active means "alive", and not "has packets".
		 */
	case CONN_ACTIVE:
		if (!c->num_requests) break;

		if (c->idle_ev) (void) fr_event_timer_delete(c->thread->el, &c->idle_ev);
		return;

		/*
		 *	If it's zombie or pinging, we don't set an
		 *	idle timer.
		 */
	case CONN_ZOMBIE:
	case CONN_STATUS_CHECKS:
		if (c->idle_ev) (void) fr_event_timer_delete(c->thread->el, &c->idle_ev);
		return;

	}

	/*
	 *	We've already set an idle timeout.  Don't do it again.
	 */
	if (c->idle_ev) return;

	gettimeofday(&when, NULL);
	when.tv_usec += c->inst->parent->idle_timeout.tv_usec;
	when.tv_sec += when.tv_usec / USEC;
	when.tv_usec %= USEC;

	when.tv_sec += c->inst->parent->idle_timeout.tv_sec;
	when.tv_sec += 1;

	if (timercmp(&when, &c->idle_timeout, >)) {
		when.tv_sec--;
		c->idle_timeout = when;

		DEBUG("%s setting idle timeout to +%pV for connection %s",
		      c->inst->parent->name, fr_box_timeval(c->inst->parent->idle_timeout), c->name);
		if (fr_event_timer_insert(c, c->thread->el, &c->idle_ev, &c->idle_timeout, conn_idle_timeout, c) < 0) {
			ERROR("%s failed inserting idle timeout for connection %s",
			      c->inst->parent->name, c->name);
		}
	}
}


/** Set the socket to "nothing to write"
 *
 *  But keep the read event open, just in case the other end sends us
 *  data  That way we can process it.
 *
 * @param[in] c		Connection data structure
 */
static void fd_idle(rlm_radius_udp_connection_t *c)
{
	rlm_radius_udp_thread_t	*t = c->thread;

	c->pending = false;
	DEBUG3("Marking socket %s as idle", c->name);
	if (fr_event_fd_insert(c->conn, t->el, c->fd,
			       conn_read, NULL, conn_error, c) < 0) {
		PERROR("Failed inserting FD event");
		talloc_free(c);
	}

	conn_idle(c);
}

/** Set the socket to active
 *
 * We have messages we want to send, so need to know when the socket is writable.
 *
 * @param[in] c		Connection data structure
 */
static void fd_active(rlm_radius_udp_connection_t *c)
{
	rlm_radius_udp_thread_t	*t = c->thread;

	c->pending = true;
	DEBUG3("%s activating connection %s",
	       c->inst->parent->name, c->name);

	/*
	 *	If we're writing to the connection, it's not idle.
	 */
	if (c->idle_ev) (void) fr_event_timer_delete(c->thread->el, &c->idle_ev);

	if (fr_event_fd_insert(c->conn, t->el, c->fd,
			       conn_read, conn_writable, conn_error, c) < 0) {
		PERROR("Failed inserting FD event");
		talloc_free(c);
	}
}


/** Close a socket due to zombie timeout
 *
 */
static void conn_zombie_timeout(UNUSED fr_event_list_t *el, UNUSED struct timeval *now, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);

	DEBUG("%s TIMER - zombie timeout for connection %s",
	      c->inst->parent->name, c->name);

	/*
	 *	If we're doing Status-Server checks, add the
	 *	pre-created packet to the outgoing queue.  It will be
	 *	sent as soon as the connection is active.  The
	 *	conn_write() routine will also take care of setting
	 *	the correct timers / timeout functions.
	 */
	if (c->status_u) {
		(void) fr_heap_insert(c->queued, c->status_u);
		if (!c->pending) fd_active(c);
		return;
	}

	DEBUG2("No status_check, closing connection");
	talloc_free(c);
}


/** A connection is "zombie"
 *
 */
static void conn_zombie(rlm_radius_udp_connection_t *c)
{
	struct timeval when;

	/*
	 *	Already zombie, don't do anything
	 */
	if (c->state == CONN_ZOMBIE) return;

	rad_assert(c->state != CONN_STATUS_CHECKS);

	/*
	 *	Remember when we became a zombie, and move the
	 *	connection from the active heap to the zombie list.
	 */
	gettimeofday(&when, NULL);

	// @todo - hysteresis... if we're close to c->mrs_time, don't be zombie...

	(void) fr_heap_extract(c->thread->active, c);
	fr_dlist_insert_head(&c->thread->zombie, &c->entry);
	c->state = CONN_ZOMBIE;
	c->zombie_start = when;

	fr_timeval_add(&when, &when, &c->inst->parent->zombie_period);
	DEBUG("%s setting to zombie for connection %s",
	      c->inst->parent->name, c->name);

	if (fr_event_timer_insert(c, c->thread->el, &c->zombie_ev, &when, conn_zombie_timeout, c) < 0) {
		ERROR("%s failed inserting idle timeout for connection %s",
		      c->inst->parent->name, c->name);
	}
}


/** Connection errored
 *
 */
static void conn_error(fr_event_list_t *el, UNUSED int fd, UNUSED int flags, int fd_errno, void *uctx)
{
	fr_dlist_t *entry;
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);

	ERROR("%s Failed new connection %s: %s",
	      c->inst->parent->name, c->name, fr_syserror(fd_errno));

	/*
	 *	Stop all Status-Server checks
	 */
	if (c->status_u) talloc_free(c->status_u);

	/*
	 *	Delete all timers associated with the connection.
	 */
	(void) fr_event_timer_delete(el, &c->idle_ev);
	(void) fr_event_timer_delete(el, &c->zombie_ev);

	/*
	 *	Move "sent" packets back to the connection queue, and
	 *	remove their retransmission timers.
	 *
	 *	@todo - ensure that the retransmission is independent
	 *	of which connection the packet is sent on.  This means
	 *	keeping the various timers in 'u' instead of in 'rr'.
	 */
	while ((entry = FR_DLIST_FIRST(c->sent)) != NULL) {
		rlm_radius_udp_request_t *u;

		u = fr_ptr_to_type(rlm_radius_udp_request_t, entry, entry);

		(void) rr_track_delete(c->id, u->rr);
		u->rr = NULL;
		u->c = NULL;
		fr_dlist_remove(&u->entry);

		(void) fr_heap_insert(c->queued, u);
		c->pending = true;
	}

	/*
	 *	Something bad happened... Fix it...
	 */
	fr_connection_reconnect(c->conn);
}


static void mod_finished_request(rlm_radius_udp_connection_t *c, rlm_radius_udp_request_t *u)
{
	/*
	 *	Delete the tracking table entry, and remove the
	 *	request from the "sent" list for this connection.
	 */
	(void) rr_track_delete(c->id, u->rr);
	u->rr = NULL;
	u->c = NULL;
	fr_dlist_remove(&u->entry);
	rad_assert(c->num_requests > 0);
	c->num_requests--;

	conn_idle(c);

	unlang_resumable(u->link->request);
}

/** Turn a reply code into a module rcode;
 *
 */
static rlm_rcode_t code2rcode[FR_MAX_PACKET_CODE] = {
	[FR_CODE_ACCESS_ACCEPT] = RLM_MODULE_OK,
	[FR_CODE_ACCESS_CHALLENGE] = RLM_MODULE_UPDATED,
	[FR_CODE_ACCESS_REJECT] = RLM_MODULE_REJECT,

	[FR_CODE_ACCOUNTING_RESPONSE] = RLM_MODULE_OK,

	[FR_CODE_COA_ACK] = RLM_MODULE_OK,
	[FR_CODE_COA_NAK] = RLM_MODULE_REJECT,

	[FR_CODE_DISCONNECT_ACK] = RLM_MODULE_OK,
	[FR_CODE_DISCONNECT_NAK] = RLM_MODULE_REJECT,
};


/** If we get a reply, the request must come from one of a small
 * number of packet types.
 */
static FR_CODE allowed_replies[FR_MAX_PACKET_CODE] = {
	[FR_CODE_ACCESS_ACCEPT] = FR_CODE_ACCESS_REQUEST,
	[FR_CODE_ACCESS_CHALLENGE] = FR_CODE_ACCESS_REQUEST,
	[FR_CODE_ACCESS_REJECT] = FR_CODE_ACCESS_REQUEST,

	[FR_CODE_ACCOUNTING_RESPONSE] = FR_CODE_ACCOUNTING_REQUEST,

	[FR_CODE_COA_ACK] = FR_CODE_COA_REQUEST,
	[FR_CODE_COA_NAK] = FR_CODE_COA_REQUEST,

	[FR_CODE_DISCONNECT_ACK] = FR_CODE_DISCONNECT_REQUEST,
	[FR_CODE_DISCONNECT_NAK] = FR_CODE_DISCONNECT_REQUEST,
};


/** Read reply packets.
 *
 */
static void conn_read(fr_event_list_t *el, int fd, UNUSED int flags, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);
	rlm_radius_request_t *rr;
	rlm_radius_link_t *link;
	rlm_radius_udp_request_t *u;
	int code;
	decode_fail_t reason;
	size_t packet_len;
	ssize_t data_len;
	REQUEST *request = NULL;
	uint8_t original[20];

	DEBUG3("%s reading data for connection %s",
	       c->inst->parent->name, c->name);

redo:
	/*
	 *	Drain the socket of all packets.  If we're busy, this
	 *	saves a round through the event loop.  If we're not
	 *	busy, a few extra system calls don't matter.
	 */
	data_len = read(fd, c->buffer, c->buflen);
	if (data_len == 0) return;

	if (data_len < 0) {
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) return;

		conn_error(el, fd, 0, errno, c);
		return;
	}

	/*
	 *	Replicating?  Drain the socket, but ignore all responses.
	 */
	 if (c->inst->replicate) goto redo;

	packet_len = data_len;
	if (!fr_radius_ok(c->buffer, &packet_len, false, &reason)) {
		DEBUG("%s Ignoring malformed packet", c->inst->parent->name);

		if (DEBUG_ENABLED3) {
		}
		goto redo;
	}

	if (DEBUG_ENABLED3) {
		DEBUG("rlm_radius read packet");
		fr_radius_print_hex(fr_log_fp, c->buffer, packet_len);
	}

	rr = rr_track_find(c->id, c->buffer[1], NULL);
	if (!rr) {
		DEBUG("%s Ignoring response to request we did not send", c->inst->parent->name);
		goto redo;
	}

	link = rr->link;
	u = link->request_io_ctx;
	request = link->request; /* may be NULL for Status-Server */

	original[0] = rr->code;
	original[1] = 0;	/* not looked at by fr_radius_verify() */
	original[2] = 0;
	original[3] = 0;
	memcpy(original + 4, rr->vector, sizeof(rr->vector));

	if (fr_radius_verify(c->buffer, original,
			     (uint8_t const *) c->inst->secret, strlen(c->inst->secret)) < 0) {
		if (request) RDEBUG("%s Ignoring response with invalid signature", c->inst->parent->name);
		return;
	}

	/*
	 *	Stop all retransmissions.
	 */
	if (u->rr->ev) (void) fr_event_timer_delete(c->thread->el, &u->rr->ev);

	switch (c->state) {
	default:
		rad_assert(0 == 1);
		break;

	case CONN_STATUS_CHECKS:
		// @todo - require N responses before marking it alive.  See RFC 3539 for details
		fr_dlist_remove(&c->entry);
		break;

	case CONN_ZOMBIE:
		fr_dlist_remove(&c->entry);
		break;

	case CONN_FULL:
		fr_dlist_remove(&c->entry);
		rad_assert(c->id->num_free > 0);
		break;

	case CONN_ACTIVE:
		(void) fr_heap_extract(c->thread->active, c);
		break;
	}

	/*
	 *	Track the Most Recently Started with reply
	 */
	if (timercmp(&rr->start, &c->mrs_time, >)) {
		c->mrs_time = rr->start;
	}

	(void) fr_heap_insert(c->thread->active, c);
	c->state = CONN_ACTIVE;

	code = c->buffer[0];

	/*
	 *	Set request return code based on the packet type.
	 *	Note that we don't care what the sent packet is, we
	 *	presume that the reply is correct for the request,
	 *	because it has been successfully verified.  The reply
	 *	packet code only affects the module return code,
	 *	nothing else.
	 *
	 *	Protocol-Error is special.  It goes through it's own
	 *	set of checks.
	 */
	if (code == FR_CODE_PROTOCOL_ERROR) {
		uint8_t const *attr, *end;

		end = c->buffer + packet_len;
		link->rcode = RLM_MODULE_INVALID;

		for (attr = c->buffer + 20;
		     attr < end;
		     attr += attr[1]) {
			/*
			 *	@todo - fix dict2header code to handle
			 *	extended OIDs
			 */
			if (attr[0] != FR_EXTENDED_ATTRIBUTE_1) continue;

			/*
			 *	ATTR + LEN + EXT-Attr + uint32
			 */
			if (attr[1] != 7) continue;

			/*
			 *	@todo - fix dict2header code to handle
			 *	extended OIDs
			 */
			if (attr[2] != 4) continue;

			/*
			 *	Has to be an 8-bit number.
			 */
			if ((attr[3] != 0) ||
			    (attr[4] != 0) ||
			    (attr[5] != 0)) {
				if (request) RDEBUG("Original-Packet-Code has invalid value > 255");
				break;
			}

			/*
			 *	This has to match.  We don't currently
			 *	multiplex different codes with the
			 *	same IDs on connections.  So this
			 *	check is just for RFC compliance, and
			 *	for sanity.
			 */
			if (attr[6] != u->code) {
				if (request) RDEBUG("Original-Packet-Code %d does not match original code %d",
						    attr[6], u->code);
				break;
			}

			/*
			 *	Allow the Protocol-Error response,
			 *	which returns "fail".
			 */
			link->rcode = RLM_MODULE_FAIL;
			break;
		}

	} else if (!code || (code >= FR_MAX_PACKET_CODE)) {
		if (request) RDEBUG("Unknown reply code %d", code);
		link->rcode = RLM_MODULE_INVALID;

		/*
		 *	Different debug message.  The packet is within
		 *	the known bounds, but is one we don't handle.
		 */
	} else if (!code2rcode[code]) {
		if (request) RDEBUG("Invalid reply code %s", fr_packet_codes[code]);
		link->rcode = RLM_MODULE_INVALID;


		/*
		 *	Status-Server packets can accept all possible replies.
		 */
	} else if (u->code == FR_CODE_STATUS_SERVER) {
		link->rcode = code2rcode[code];

		/*
		 *	The reply is a known code, but isn't
		 *	appropriate for the request packet type.
		 */
	} else if (allowed_replies[code] != (FR_CODE) u->code) {
		rad_assert(request != NULL);

		RDEBUG("Invalid reply code %s to request packet %s",
		       fr_packet_codes[code], fr_packet_codes[u->code]);
		link->rcode = RLM_MODULE_INVALID;

		/*
		 *	<whew>, it's OK.  Choose the correct module
		 *	rcode based on the reply code.  This is either
		 *	OK for an ACK, or FAIL for a NAK.
		 */
	} else {
		VALUE_PAIR *vp = NULL;

		link->rcode = code2rcode[code];

		/*
		 *	Decode the attributes, in the context of the reply.
		 */
		if (fr_radius_decode(request->reply, c->buffer, packet_len, original,
				     c->inst->secret, 0, &vp) < 0) {
			RDEBUG("Failed decoding attributes for packet");
			fr_pair_list_free(&vp);
			link->rcode = RLM_MODULE_INVALID;
			goto done;
		}

		RDEBUG("%s - received %s ID %d length %ld reply packet from connection %s",
		       c->inst->parent->name, fr_packet_codes[code], code, packet_len, c->name);
		rdebug_pair_list(L_DBG_LVL_2, request, vp, NULL);

		/*
		 *	@todo - make this programmatic?  i.e. run a
		 *	separate policy which updates the reply.
		 *
		 *	This is why I wanted to have "recv
		 *	Access-Accept" policies...  so the user could
		 *	programatically decide which attributes to add.
		 */

		fr_pair_add(&request->reply->vps, vp);
	}

done:
	rad_assert(request != NULL);
	rad_assert(request->reply != NULL);

	/*
	 *	We received the response to a Status-Server
	 *	check.
	 */
	if (c->status_u == u) {
		/*
		 *	Delete the reply, but leave the request VPs in
		 *	place.
		 */
#ifdef __clang_analyzer__
		if (request && request->reply)
#endif
			fr_pair_list_free(&request->reply->vps);

	} else {
		/*
		 *	It's a normal request.  Mark it as finished.
		 */
		mod_finished_request(c, u);
	}

	/*
	 *	We've read a packet, reset the idle timers.
	 */
	conn_idle(c);

	goto redo;
}

/** Deal with per-request timeouts for Status-Server
 *
 *  Note that we update the packet in place, as it re-uses the same
 *  ID, and it doesn't grow in size.
 */
static void status_check_timeout(UNUSED fr_event_list_t *el, struct timeval *now, void *uctx)
{
	int rcode;
	rlm_radius_udp_request_t *u = uctx;
	rlm_radius_udp_connection_t *c = u->c;
	REQUEST *request;

	/*
	 *	This is here instead of in conn_write(), because we
	 *	may need to shut down the connection.  It's better to
	 *	do that here than to overload conn_write(), which is
	 *	already a bit complex.
	 */
	rcode = rr_track_retry(c->id, u->rr, c->thread->el, status_check_timeout, u, &c->inst->parent->retry[u->code], now);
	if (rcode < 0) {
		/*
		 *	Failed inserting event... the request is done.
		 */
		conn_error(c->thread->el, c->fd, 0, EINVAL, c);
		return;
	}

	request = u->link->request;
	if (rcode == 0) {
		RDEBUG("No response to status check,  Marking connection dead");
		talloc_free(c);
		return;
	}

	/*
	 *	Link it into the connection queue for retransmission.
	 */
	fr_dlist_remove(&u->entry);
	(void) fr_heap_insert(c->queued, u);
	if (!c->pending) fd_active(c);
}


/** Deal with per-request timeouts for transmissions, etc.
 *
 */
static void response_timeout(UNUSED fr_event_list_t *el, struct timeval *now, void *uctx)
{
	int rcode;
	rlm_radius_udp_request_t *u = uctx;
	rlm_radius_udp_connection_t *c = u->c;
	REQUEST *request;

	rcode = rr_track_retry(c->id, u->rr, c->thread->el, response_timeout, u, &c->inst->parent->retry[u->code], now);
	if (rcode < 0) {
		/*
		 *	Failed inserting event... the request is done.
		 */
		mod_finished_request(c, u);
		return;
	}

	request = u->link->request;
	if (rcode == 0) {
		conn_zombie(c);
		RDEBUG("No response to proxied request");
		mod_finished_request(c, u);
		return;
	}

	/*
	 *	@todo - RADIUS layer fixups!
	 *
	 *	For accounting packets, update Acct-Delay-Time <sigh>
	 */

	RDEBUG("Retransmitting request.  Expecting response within %d.%06ds",
	       u->rr->rt / USEC, u->rr->rt % USEC);
	rcode = write(c->fd, u->packet, u->packet_len);
	if (rcode < 0) {
		if (errno == EWOULDBLOCK) {
			return;
		}

		/*
		 *	We have to re-encode the packet, so
		 *	don't bother copying it to 'u'.
		 */
		conn_error(c->thread->el, c->fd, 0, errno, c);
		return;
	}
}


/** Write a packet to a connection
 *
 * @param c the conneciton
 * @param u the udp_request_t connecting everything
 * @return
 *	- <0 on error
 *	- 0 should retry the write later
 *	- 1 the packet was successfully written to the socket, and we wait for a reply
 *	- 2 the packet was replicated to the socket, and should be resumed immediately.
 */
static int conn_write(rlm_radius_udp_connection_t *c, rlm_radius_udp_request_t *u)
{
	int rcode;
	size_t buflen;
	ssize_t packet_len;
	uint8_t *msg = NULL;
	bool require_ma = false;
	int proxy_state = 6;
	REQUEST *request;
	char const *module_name;

	rad_assert(c->inst->parent->allowed[u->code] || (u == c->status_u));
	if (c->idle_ev) (void) fr_event_timer_delete(c->thread->el, &c->idle_ev);

	request = u->link->request;

	/*
	 *	Make sure that we print out the actual encoded value
	 *	of the Message-Authenticator attribute.  If the caller
	 *	asked for one, delete theirs (which has a bad value),
	 *	and remember to add one manually.
	 */
	if (fr_pair_find_by_num(request->packet->vps, 0, FR_MESSAGE_AUTHENTICATOR, TAG_ANY)) {
		require_ma = true;
		fr_pair_delete_by_num(&request->packet->vps, 0, FR_MESSAGE_AUTHENTICATOR, TAG_ANY);
	}

	/*
	 *	All proxied Access-Request packets MUST have a
	 *	Message-Authenticator, otherwise they're insecure.
	 *	Same goes for Status-Server.
	 *
	 *	And we set the authentication vector to a random
	 *	number...
	 */
	if ((u->code == FR_CODE_ACCESS_REQUEST) ||
	    (u->code == FR_CODE_STATUS_SERVER)) {
		size_t i;
		uint32_t hash, base;

		require_ma = true;

		base = fr_rand();
		for (i = 0; i < AUTH_VECTOR_LEN; i += sizeof(uint32_t)) {
			hash = fr_rand() ^ base;
			memcpy(c->buffer + 4 + i, &hash, sizeof(hash));
		}
	}

	/*
	 *	Every status check packet has an Event-Timestamp.
	 *	Which changes every time we send a packet, but it
	 *	doesn't have Proxy-State.
	 */
	if (u == c->status_u) {
		VALUE_PAIR *vp;

		proxy_state = 0;
		vp = fr_pair_find_by_num(request->packet->vps, 0, FR_EVENT_TIMESTAMP, TAG_ANY);
		vp->vp_uint32 = time(NULL);
	}

	/*
	 *	Leave room for the Message-Authenticator.
	 */
	if (require_ma) {
		buflen = c->buflen - 18;
	} else {
		buflen = c->buflen;
	}

	/*
	 *	Encode it, leaving room for Proxy-State, too.
	 */
	packet_len = fr_radius_encode(c->buffer, buflen - proxy_state, NULL,
				      c->inst->secret, 0, u->code, u->rr->id,
				      request->packet->vps);
	if (packet_len <= 0) return -1;

	RDEBUG("sending %s ID %d length %ld over connection %s",
	       fr_packet_codes[u->code], u->rr->id, packet_len, c->name);

	/*
	 *	This hack cleans up the debug output a bit.
	 */
	module_name = request->module;
	request->module = NULL;
	rdebug_pair_list(L_DBG_LVL_2, request, request->packet->vps, NULL);

	/*
	 *	Might have been sent and then given up
	 *	on... free the raw data.
	 */
	if (u->packet) TALLOC_FREE(u->packet);

	/*
	 *	Add Proxy-State to the tail end of the packet.
	 *	We need to add it here, and NOT in
	 *	request->packet->vps, because multiple modules
	 *	may be sending the packets at the same time.
	 *
	 *	Note that the length check will always pass, due to
	 *	the buflen manipulation done above.
	 */
	if (proxy_state) {
		uint8_t *attr = c->buffer + packet_len;
		int hdr_len;

		rad_assert((size_t) (packet_len + 6) <= c->buflen);

		attr[0] = FR_PROXY_STATE;
		attr[1] = 6;
		memcpy(attr + 2, &c->inst->parent->proxy_state, 4);

		hdr_len = (c->buffer[2] << 8) | (c->buffer[3]);
		hdr_len += 6;
		c->buffer[2] = (hdr_len >> 8) & 0xff;
		c->buffer[3] = hdr_len & 0xff;

		if (radlog_debug_enabled(L_DBG, L_DBG_LVL_2, request)) {
			RINDENT();
			RDEBUG2("&Proxy-State := 0x%08x", c->inst->parent->proxy_state);
			REXDENT();
		}

		packet_len += 6;
	}

	/*
	 *	Add Message-Authenticator manually.
	 *
	 *	Note that the length check will always pass, due to
	 *	the buflen manipulation done above.
	 */
	if (require_ma &&
	    ((size_t) (packet_len + 18) <= c->buflen)) {
		int hdr_len;

		msg = c->buffer + packet_len;

		msg[0] = FR_MESSAGE_AUTHENTICATOR;
		msg[1] = 18;
		memset(msg + 2, 0, 16);

		hdr_len = (c->buffer[2] << 8) | (c->buffer[3]);
		hdr_len += 18;
		c->buffer[2] = (hdr_len >> 8) & 0xff;
		c->buffer[3] = hdr_len & 0xff;

		packet_len += 18;
	}

	if (fr_radius_sign(c->buffer, NULL, (uint8_t const *) c->inst->secret,
			   strlen(c->inst->secret)) < 0) {
		request->module = module_name;
		ERROR("Failed signing packet");
		conn_error(c->thread->el, c->fd, 0, errno, c);
		return -1;
	}

	/*
	 *	Print out the actual value of the Message-Authenticator attribute
	 */
	if (msg && radlog_debug_enabled(L_DBG, L_DBG_LVL_2, request)) {
		char msg_buf[2 * 16 + 1];

		fr_bin2hex(msg_buf, msg + 2, msg[1] - 2);

		RINDENT();
		RDEBUG2("&Message-Authenticator := %s", msg_buf);
		REXDENT();
	}

	if (DEBUG_ENABLED3) {
		RDEBUG("rlm_radius encode packet");
		fr_radius_print_hex(fr_log_fp, c->buffer, packet_len);
	}

	request->module = module_name;

	/*
	 *	Write the packet to the socket.  If it blocks,
	 *	stop dequeueing packets.
	 */
	rcode = write(c->fd, c->buffer, packet_len);
	if (rcode < 0) {
		if (errno == EWOULDBLOCK) {
			MEM(u->packet = talloc_memdup(u, c->buffer, packet_len));
			u->packet_len = packet_len;
			return 0;
		}

		/*
		 *	We have to re-encode the packet, so
		 *	don't bother copying it to 'u'.
		 */
		conn_error(c->thread->el, c->fd, 0, errno, c);
		return 0;
	}

	/*
	 *	We're replicating, so we don't care about the
	 *	responses.  Don't do any retransmission
	 *	timers, etc.
	 */
	if (c->inst->replicate) {
		return 1;
	}

	/*
	 *	Start the retransmission timers.
	 */
	u->link->time_sent = fr_time();
	fr_time_to_timeval(&u->rr->start, u->link->time_sent);

	if (proxy_state) {
		c->num_requests++;

		/*
		 *	Only copy the packet if we're not replicating,
		 *	and we're not doing Status-Server checks.
		 *
		 *	@todo - don't do this for accounting packets,
		 *	which will need Acct-Delay-Time to be updated
		 */
		MEM(u->packet = talloc_memdup(u, c->buffer, packet_len));
		u->packet_len = packet_len;

		RDEBUG("Proxying request.  Expecting response within %d.%06ds",
		       u->rr->rt / USEC, u->rr->rt % USEC);

		if (rr_track_start(c->id, u->rr, c->thread->el, response_timeout, u, &c->inst->parent->retry[u->code]) < 0) {
			RDEBUG("Failed starting retransmit tracking");
			return -1;
		}

	} else if (u->rr->count == 0) {
		if (rr_track_start(c->id, u->rr, c->thread->el, status_check_timeout, u, &c->inst->parent->retry[u->code]) < 0) {
			RDEBUG("Failed starting retransmit tracking");
			return -1;
		}

		RDEBUG("Sending %s status check.  Expecting response within %d.%06ds",
		       fr_packet_codes[u->code],
		       u->rr->rt / USEC, u->rr->rt % USEC);

	} else {
		RDEBUG("Retransmitting %s status check.  Expecting response within %d.%06ds",
		       fr_packet_codes[u->code],
		       u->rr->rt / USEC, u->rr->rt % USEC);
	}

	fr_dlist_remove(&u->entry);
	fr_dlist_insert_tail(&c->sent, &u->entry);

	return 1;
}

/** There's space available to write data, so do that...
 *
 */
static void conn_writable(UNUSED fr_event_list_t *el, UNUSED int fd, UNUSED int flags, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);
	rlm_radius_udp_request_t *u;
	bool pending;

	rad_assert(c->idle_ev == NULL); /* if it's writable and we're writing, it can't be idle */

	DEBUG3("%s writing packets for connection %s",
	       c->inst->parent->name, c->name);

	/*
	 *	Clear our backlog
	 */
	while ((u = fr_heap_pop(c->queued)) != NULL) {
		int rcode;

		rcode = conn_write(c, u);

		// @todo - do something intelligent on error..
		if (rcode <= 0) break;

		if (rcode == 1) continue;

		/*
		 *	Was replicated: can resume it immediately.
		 */
		unlang_resumable(u->link->request);
	}

	/*
	 *	Check if we have to enable or disable writing on the socket.
	 */
	pending = (fr_heap_num_elements(c->queued) > 0);
	if (!pending && c->pending) {
		/*
		 *	The queue is empty, and we apparently just
		 *	emptied it.  Set the FD to idle.
		 */
		fd_idle(c);
	}

	/*
	 *	This check is here only for mod_push(), which
	 *	calls us when there are no packets pending on
	 *	a socket.  If the connection is writable, and
	 *	the write succeeds, and there's nothing more
	 *	to write, we don't need to call fd_active().
	 */
	else if (pending && !c->pending) {
		fd_active(c);
	}
}

/** Shutdown/close a file descriptor
 *
 */
static void conn_close(int fd, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);

	if (c->idle_ev) fr_event_timer_delete(c->thread->el, &c->idle_ev);

	DEBUG("%s closing connection %s",
	      c->inst->parent->name, c->name);

	if (shutdown(fd, SHUT_RDWR) < 0) {
		DEBUG3("%s failed shutting down connection %s: %s",
		       c->inst->parent->name, c->name, fr_syserror(errno));
	}

	if (close(fd) < 0) {
		DEBUG3("%s failed closing connection %s: %s",
		       c->inst->parent->name, c->name, fr_syserror(errno));
	}

	c->fd = -1;
}

/** Free an rlm_radius_udp_request_t
 *
 *  Unlink the packet from the connection, and remove any tracking
 *  entries.
 */
static int udp_request_free(rlm_radius_udp_request_t *u)
{
	fr_dlist_remove(&u->entry);

	if (u->rr) {
		(void) rr_track_delete(u->c->id, u->rr);
		u->rr = NULL;
	}

	return 0;
}

/** Free the status-check rlm_radius_udp_request_t
 *
 *  Unlink the packet from the connection, and remove any tracking
 *  entries.
 */
static int status_udp_request_free(rlm_radius_udp_request_t *u)
{
	rlm_radius_udp_connection_t *c = u->c;

	DEBUG3("%s freeing status check ID %d on connection %s",
	       c->inst->parent->name, u->rr->id, c->name);
	c->status_u = NULL;

	return udp_request_free(u);
}


/** Process notification that fd is open
 *
 */
static fr_connection_state_t conn_open(UNUSED fr_event_list_t *el, UNUSED int fd, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);
	rlm_radius_udp_thread_t *t = c->thread;
	char src_buf[128], dst_buf[128];

	fr_value_box_snprint(src_buf, sizeof(src_buf), fr_box_ipaddr(c->src_ipaddr), 0);
	fr_value_box_snprint(dst_buf, sizeof(dst_buf), fr_box_ipaddr(c->dst_ipaddr), 0);

	talloc_const_free(c->name);
	c->name = talloc_asprintf(c, "proto udp local %s port %u remote %s port %u",
				  src_buf, c->src_port,
				  dst_buf, c->dst_port);
	c->state = CONN_OPENING;

	/*
	 *	Connection is "active" now.  i.e. we prefer the newly
	 *	opened connection for sending packets.
	 *
	 *	@todo - connection negotiation via Status-Server
	 */
	gettimeofday(&c->mrs_time, NULL);

	DEBUG("%s opened new connection %s",
	      c->inst->parent->name, c->name);

	/*
	 *	Remove the connection from the "opening" list, and add
	 *	it to the "active" list.
	 */
	rad_assert(c->state == CONN_OPENING);
	fr_dlist_remove(&c->entry);
	fr_heap_insert(t->active, c);
	c->state = CONN_ACTIVE;

	/*
	 *	Status-Server checks.  Manually build the packet, and
	 *	all of it's associated glue.
	 */
	if (c->inst->parent->status_check) {
		rlm_radius_link_t *link;
		rlm_radius_udp_request_t *u;
		REQUEST *request;

		link = talloc_zero(c, rlm_radius_link_t);
		u = talloc_zero(c, rlm_radius_udp_request_t);
		request = request_alloc(link);
		request->async = talloc_zero(request, fr_async_t);

		// @todo - if we call unlang, we need to set a whole lot more... see worker.c
		request->el = c->thread->el;
		request->packet = fr_radius_alloc(request, false);
		request->reply = fr_radius_alloc(request, false);

		/*
		 *	Create the packet contents.
		 *
		 *	@todo - different packet contents for
		 *	Access-Request, Accounting-Request, etc.
		 */
		pair_make_request("NAS-Identifier", "status check - are you alive?", T_OP_EQ);
		pair_make_request("Event-Timestamp", "0", T_OP_EQ);

		/*
		 *	Initialize the link.  Note that we don't set
		 *	destructors.
		 */
		FR_DLIST_INIT(link->entry);
		link->request = request;
		link->request_io_ctx = u;

		/*
		 *	Unitialize the UDP link.
		 */
		FR_DLIST_INIT(u->entry);
		u->code = c->inst->parent->status_check;
		request->packet->code = u->code;
		u->c = c;
		u->link = link;

		/*
		 *	Reserve a permanent ID for the packet.  This
		 *	is because we need to be able to send an ID on
		 *	demand.  If the proxied packets use all of the
		 *	IDs, then we can't send a Status-Server check.
		 */
		u->rr = rr_track_alloc(c->id, request, u->code, link);
		if (!u->rr) {
			ERROR("%s failed allocating status_check ID for new connection %s",
			      c->inst->parent->name, c->name);
			talloc_free(u);
			talloc_free(link);

		} else {
			DEBUG2("%s allocated %s ID %u for status checks on connection %s",
			       c->inst->parent->name, fr_packet_codes[u->code], u->rr->id, c->name);
			talloc_set_destructor(u, status_udp_request_free);
			c->status_u = u;
		}
	}

	/*
	 *	Now that we're open, also push pending requests from
	 *	the main thread queue onto the queue for this
	 *	connection.
	 */
	if (t->pending) mod_clear_backlog(t);

	/*
	 *	If we have data pending, add the writable event immediately
	 */
	if (c->pending) {
		fd_active(c);
	} else {
		fd_idle(c);
	}

	return FR_CONNECTION_STATE_CONNECTED;
}


/** Initialise a new outbound connection
 *
 * @param[out] fd_out	Where to write the new file descriptor.
 * @param[in] uctx	A #rlm_radius_thread_t.
 */
static fr_connection_state_t conn_init(int *fd_out, void *uctx)
{
	int fd;
#if defined (SO_RCVBUF) || defined (SO_SNDBUF)
	int opt;
#endif
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);
	char src_buf[128], dst_buf[128];

	/*
	 *	Open the outgoing socket.
	 *
	 *	@todo - pass src_port, and remove later call to fr_socket_bind()
	 *	which does return the src_port, but doesn't set the "don't fragment" bit.
	 */
	fd = fr_socket_client_udp(&c->src_ipaddr, &c->dst_ipaddr, c->dst_port, true);
	if (fd < 0) {
		DEBUG("%s failed opening socket: %s",
		      c->inst->parent->name, fr_strerror());
		return FR_CONNECTION_STATE_FAILED;
	}

#if 0
	if (fr_socket_bind(fd, &io->src_ipaddr, &io->src_port, inst->interface) < 0) {
		DEBUG("Failed binding RADIUS client UDP socket: %s FD %d %pV port %u interface %s", fr_strerror(), fd, fr_box_ipaddr(io->src_ipaddr),
			io->src_port, inst->interface);
		return FR_CONNECTION_STATE_FAILED;
	}
#endif

	/*
	 *	Set the connection name.
	 */
	fr_value_box_snprint(src_buf, sizeof(src_buf), fr_box_ipaddr(c->src_ipaddr), 0);
	fr_value_box_snprint(dst_buf, sizeof(dst_buf), fr_box_ipaddr(c->dst_ipaddr), 0);

	c->name = talloc_asprintf(c, "connecting proto udp from %s to %s port %u",
				  src_buf,
				  dst_buf, c->dst_port);

#ifdef SO_RCVBUF
	opt = c->inst->recv_buff;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &opt, sizeof(int)) < 0) {
		WARN("Failed setting 'recv_buf': %s", fr_syserror(errno));
	}
#endif

#ifdef SO_SNDBUF
	opt = c->inst->send_buff;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(int)) < 0) {
		WARN("Failed setting 'send_buf': %s", fr_syserror(errno));
	}
#endif

	c->fd = fd;

	// @todo - initialize the tracking memory, etc.
	// i.e. histograms (or hyperloglog) of packets, so we can see
	// which connections / home servers are fast / slow.

	*fd_out = fd;

	return FR_CONNECTION_STATE_CONNECTING;
}

/** Free the connection, and return requests to the thread queue
 *
 */
static int conn_free(rlm_radius_udp_connection_t *c)
{
	fr_dlist_t *entry;
	rlm_radius_udp_request_t *u;
	rlm_radius_udp_thread_t *t = c->thread;

	talloc_free(c->conn);
	c->conn = NULL;

	talloc_free_children(c); /* clears out FD events, timers, etc. */

	/*
	 *	Status-Server checks remain with this connection, and
	 *	don't get sent back to the main thread queue.
	 */
	if (c->status_u) {
		talloc_free(c->status_u);
		c->status_u = NULL;
	}

	/*
	 *	Move "sent" packets back to the main thread queue
	 */
	while ((entry = FR_DLIST_FIRST(c->sent)) != NULL) {
		u = fr_ptr_to_type(rlm_radius_udp_request_t, entry, entry);

		/*
		 *	Don't bother freeing individual entries.  They
		 *	will get deleted when c->id is free'd.
		 */
		u->rr = NULL;
		u->c = NULL;
		(void) fr_event_timer_delete(c->thread->el, &u->rr->ev);
		fr_dlist_remove(&u->entry);
		(void) fr_heap_insert(t->queued, u);
		t->pending = true;
	}

	/*
	 *	Move "queued" packets back to the main thread queue
	 */
	while ((u = fr_heap_pop(c->queued)) != NULL) {
		u->rr = NULL;
		u->c = NULL;
		(void) fr_heap_insert(t->queued, u);
		t->pending = true;
	}

	switch (c->state) {
	default:
		rad_assert(0 == 1);
		break;

	case CONN_STATUS_CHECKS:
	case CONN_OPENING:
	case CONN_FULL:
	case CONN_ZOMBIE:
		fr_dlist_remove(&c->entry);
		break;

	case CONN_ACTIVE:
		(void) fr_heap_extract(t->active, c);
		break;
	}

	return 0;
}


/** Allocate a new connection and set it up.
 *
 */
static void mod_connection_alloc(rlm_radius_udp_t *inst, rlm_radius_udp_thread_t *t)
{
	rlm_radius_udp_connection_t *c;

	c = talloc_zero(t, rlm_radius_udp_connection_t);
	c->inst = inst;
	c->thread = t;
	c->dst_ipaddr = inst->dst_ipaddr;
	c->dst_port = inst->dst_port;
	c->src_ipaddr = inst->src_ipaddr;
	c->src_port = 0;
	c->max_packet_size = inst->max_packet_size;

	c->buffer = talloc_array(c, uint8_t, c->max_packet_size);
	if (!c->buffer) {
		cf_log_err(inst->config, "%s failed allocating memory for new connection",
			   inst->parent->name);
		talloc_free(c);
		return;
	}
	c->buflen = c->max_packet_size;

	/*
	 *	Note that each connection can have AT MOST 256 packets
	 *	outstanding, no matter what the packet code.  i.e. we
	 *	use a common ID space for all packet codes sent on
	 *	this connection.
	 *
	 *	This is the same behavior as v2 and v3.  In an ideal
	 *	world, we SHOULD be able to have separate ID spaces
	 *	for each packet code.  The problem is that the replies
	 *	don't contain the original packet codes.  Which means
	 *	looking up packets by ID is difficult.
	 */
	c->id = rr_track_create(c);
	if (!c->id) {
		cf_log_err(inst->config, "%s failed allocating ID tracking for new connection",
			   inst->parent->name);
		talloc_free(c);
		return;
	}
	c->num_requests = 0;
	c->max_requests = 256;
	c->queued = fr_heap_create(queue_cmp, offsetof(rlm_radius_udp_request_t, heap_id));
	FR_DLIST_INIT(c->sent);

	c->conn = fr_connection_alloc(c, t->el, &inst->parent->connection_timeout, &inst->parent->reconnection_delay,
				      conn_init, conn_open, conn_close, inst->parent->name, c);
	if (!c->conn) {
		cf_log_err(inst->config, "%s failed allocating state handler for new connection",
			   inst->parent->name);
		return;
	}

	fr_connection_start(c->conn);

	fr_dlist_insert_head(&t->opening, &c->entry);

	talloc_set_destructor(c, conn_free);
}

/** Get a new connection...
 *
 * For now, there's only one connection.
 */
static rlm_radius_udp_connection_t *connection_get(rlm_radius_udp_thread_t *t, rlm_radius_udp_request_t *u)
{
	rlm_radius_udp_connection_t *c;

	c = fr_heap_peek(t->active);
	if (!c) return NULL;

	(void) talloc_get_type_abort(c, rlm_radius_udp_connection_t);
	rad_assert(c->state == CONN_ACTIVE);
	rad_assert(c->num_requests < c->max_requests);

	u->rr = rr_track_alloc(c->id, u->link->request, u->code, u->link);
	if (!u->rr) {
		rad_assert(0 == 1);
		return NULL;
	}

	rad_assert(u->rr->count == 0);
	u->c = c;

	fr_heap_extract(t->active, c);
	if (c->id->num_free > 0) {
		fr_heap_insert(t->active, c);
	} else {
		fr_dlist_insert_head(&t->full, &c->entry);
		c->state = CONN_FULL;
	}

	return c;
}


static void mod_clear_backlog(rlm_radius_udp_thread_t *t)
{
	rlm_radius_udp_request_t *u;
	rlm_radius_udp_connection_t *c;

	c = fr_heap_peek(t->active);
	if (!c) return;

	while ((u = fr_heap_pop(t->queued)) != NULL) {
		c = connection_get(t, u);
		if (!c) break;

		/*
		 *	Remove it from the main thread queue, and add
		 *	it to the connection queue.
		 */
		(void) fr_heap_insert(c->queued, u);

		if (!c->pending) {
			fd_active(c);
		}
	}

	/*
	 *	Update the pending flag.
	 */
	t->pending = (fr_heap_num_elements(t->queued) > 0);
}


static rlm_rcode_t mod_push(void *instance, REQUEST *request, rlm_radius_link_t *link, void *thread)
{
	int rcode;
	rlm_radius_udp_t *inst = talloc_get_type_abort(instance, rlm_radius_udp_t);
	rlm_radius_udp_thread_t *t = talloc_get_type_abort(thread, rlm_radius_udp_thread_t);
	rlm_radius_udp_request_t *u = link->request_io_ctx;
	rlm_radius_udp_connection_t *c;

	rad_assert(request->packet->code > 0);
	rad_assert(request->packet->code < FR_MAX_PACKET_CODE);

	/*
	 *	Clear the backlog before sending any new packets.
	 */
	if (t->pending) mod_clear_backlog(t);

	u->link = link;
	u->code = request->packet->code;
	FR_DLIST_INIT(u->entry);

	talloc_set_destructor(u, udp_request_free);

	/*
	 *	Get a connection.  If they're all full, try to open a
	 *	new one.
	 */
	c = connection_get(t, u);
	if (!c) {
		fr_dlist_t *entry;

		/*
		 *	Only open one new connection at a time.
		 */
		entry = FR_DLIST_FIRST(t->opening);
		if (!entry) mod_connection_alloc(inst, t);

		/*
		 *	Add the request to the backlog.  It will be
		 *	sent either when the new connection is open,
		 *	or when an existing connection has
		 *	availability.
		 */
		t->pending = true;
		(void) fr_heap_insert(t->queued, u);
		return RLM_MODULE_YIELD;
	}

	/*
	 *	There are pending requests on this connection.  Insert
	 *	the new packet into the queue, and let the event loop
	 *	call conn_writable() as necessary.
	 */
	if (c->pending) goto queue_for_write;

	/*
	 *	There are no pending packets, try to write to the
	 *	socket immediately.  If the write succeeds, we can
	 *	return the appropriate return code.
	 */
	rcode = conn_write(c, u);
	if (rcode < 0) return RLM_MODULE_FAIL;

	/*
	 *	Got EWOULDBLOCK, or other recoverable issue writing to the socket.
	 *
	 *	Insert it into the pending queue, and mark the FD as
	 *	actively trying to write.
	 */
	if (rcode == 0) {
		fd_active(c);
	queue_for_write:
		(void) fr_heap_insert(c->queued, u);
		return RLM_MODULE_YIELD;
	}

	/*
	 *	The packet was successfully written to the socket.
	 *	There are no more packets to write, so we just yield
	 *	waiting for the reply.
	 */
	if (rcode == 1) return RLM_MODULE_YIELD;

	/*
	 *	We replicated the packet, so we return "ok", and don't
	 *	care about the reply.
	 */
	return RLM_MODULE_OK;
}


/** Bootstrap the module
 *
 * Bootstrap I/O and type submodules.
 *
 * @param[in] instance	Ctx data for this module
 * @param[in] conf    our configuration section parsed to give us instance.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_bootstrap(void *instance, CONF_SECTION *conf)
{
	rlm_radius_udp_t *inst = talloc_get_type_abort(instance, rlm_radius_udp_t);

	(void) talloc_set_type(inst, rlm_radius_udp_t);
	inst->config = conf;

	return 0;
}


/** Instantiate the module
 *
 * Instantiate I/O and type submodules.
 *
 * @param[in] parent    rlm_radius_t
 * @param[in] instance	Ctx data for this module
 * @param[in] conf	our configuration section parsed to give us instance.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_instantiate(rlm_radius_t *parent, void *instance, CONF_SECTION *conf)
{
	rlm_radius_udp_t *inst = talloc_get_type_abort(instance, rlm_radius_udp_t);

	inst->parent = parent;
	inst->replicate = parent->replicate;

	/*
	 *	Ensure that we have a destination address.
	 */
	if (inst->dst_ipaddr.af == AF_UNSPEC) {
		cf_log_err(conf, "A value must be given for 'ipaddr'");
		return -1;
	}

	/*
	 *	If src_ipaddr isn't set, make sure it's INADDR_ANY, of
	 *	the same address family as dst_ipaddr.
	 */
	if (inst->src_ipaddr.af == AF_UNSPEC) {
		memset(&inst->src_ipaddr, 0, sizeof(inst->src_ipaddr));

		inst->src_ipaddr.af = inst->dst_ipaddr.af;

		if (inst->src_ipaddr.af == AF_INET) {
			inst->src_ipaddr.prefix = 32;
		} else {
			inst->src_ipaddr.prefix = 128;
		}
	}

	else if (inst->src_ipaddr.af != inst->dst_ipaddr.af) {
		cf_log_err(conf, "The 'ipaddr' and 'src_ipaddr' configuration items must be both of the same address family");
		return -1;
	}

	if (!inst->dst_port) {
		cf_log_err(conf, "A value must be given for 'port'");
		return -1;
	}

	if (inst->recv_buff_is_set) {
		FR_INTEGER_BOUND_CHECK("recv_buff", inst->recv_buff, >=, inst->max_packet_size);
		FR_INTEGER_BOUND_CHECK("recv_buff", inst->recv_buff, <=, (1 << 30));
	}

	if (inst->send_buff_is_set) {
		FR_INTEGER_BOUND_CHECK("send_buff", inst->send_buff, >=, inst->max_packet_size);
		FR_INTEGER_BOUND_CHECK("send_buff", inst->send_buff, <=, (1 << 30));
	}

	FR_INTEGER_BOUND_CHECK("max_packet_size", inst->max_packet_size, >=, 64);
	FR_INTEGER_BOUND_CHECK("max_packet_size", inst->max_packet_size, <=, 65535);

	return 0;
}


/** Instantiate thread data for the submodule.
 *
 */
static int mod_thread_instantiate(UNUSED CONF_SECTION const *cs, void *instance, fr_event_list_t *el, void *thread)
{
	rlm_radius_udp_thread_t *t = thread;

	(void) talloc_set_type(t, rlm_radius_udp_thread_t);
	t->inst = instance;
	t->el = el;

	t->pending = false;
	t->queued = fr_heap_create(queue_cmp, offsetof(rlm_radius_udp_request_t, heap_id));
	FR_DLIST_INIT(t->zombie);
	FR_DLIST_INIT(t->full);
	FR_DLIST_INIT(t->opening);

	t->active = fr_heap_create(conn_cmp, offsetof(rlm_radius_udp_connection_t, heap_id));

	mod_connection_alloc(t->inst, t);

	return 0;
}

/** Destroy thread data for the IO submodule.
 *
 */
static int mod_thread_detach(void *thread)
{
	rlm_radius_udp_thread_t *t = talloc_get_type_abort(thread, rlm_radius_udp_thread_t);
	fr_dlist_t *entry;

	if (fr_heap_num_elements(t->queued) != 0) {
		ERROR("There are still queued requests");
		return -1;
	}
	rad_assert(t->pending == false);

	/*
	 *	Free all of the heaps, lists, and sockets.
	 */
	talloc_free(t->queued);
	talloc_free(t->active);
	talloc_free_children(t);

	entry = FR_DLIST_FIRST(t->opening);
	if (entry != NULL) {
		ERROR("There are still partially open sockets");
		return -1;
	}

	return 0;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
extern fr_radius_client_io_t rlm_radius_udp;
fr_radius_client_io_t rlm_radius_udp = {
	.magic		= RLM_MODULE_INIT,
	.name		= "radius_udp",
	.inst_size	= sizeof(rlm_radius_udp_t),
	.request_inst_size = sizeof(rlm_radius_udp_request_t),
	.thread_inst_size	= sizeof(rlm_radius_udp_thread_t),

	.config		= module_config,
	.bootstrap	= mod_bootstrap,
	.instantiate	= mod_instantiate,
	.thread_instantiate = mod_thread_instantiate,
	.thread_detach	= mod_thread_detach,

	.push		= mod_push,
};
