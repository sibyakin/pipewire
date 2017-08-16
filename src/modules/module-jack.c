/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>

#include "config.h"

#include "pipewire/pipewire.h"
#include "pipewire/log.h"
#include "pipewire/interfaces.h"

#include "pipewire/core.h"
#include "pipewire/node.h"
#include "pipewire/module.h"
#include "pipewire/client.h"
#include "pipewire/resource.h"
#include "pipewire/private.h"
#include "pipewire/link.h"
#include "pipewire/node-factory.h"
#include "pipewire/data-loop.h"
#include "pipewire/main-loop.h"

#include "modules/module-jack/jack.h"
#include "modules/module-jack/jack-node.h"

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   108
#endif

#define LOCK_SUFFIX     ".lock"
#define LOCK_SUFFIXLEN  5

int segment_num = 0;

typedef bool(*demarshal_func_t) (void *object, void *data, size_t size);

struct socket {
	int fd;
	struct sockaddr_un addr;
	char lock_addr[UNIX_PATH_MAX + LOCK_SUFFIXLEN];

	struct pw_loop *loop;
	struct spa_source *source;
	struct spa_list link;
};

struct impl {
	struct pw_core *core;
	struct pw_type *t;
	struct pw_module *module;
	struct spa_list link;

        struct spa_source *timer;

	struct pw_properties *properties;

	struct spa_list socket_list;
	struct spa_list client_list;

	struct spa_loop_control_hooks hooks;

	struct jack_server server;

	struct pw_link *sink_link;

	struct {
		struct spa_list nodes;
	} rt;
};

struct client {
	struct impl *impl;
	struct spa_list link;
	struct pw_client *client;
	struct spa_hook client_listener;
	int fd;
	struct spa_source *source;
};

static bool init_socket_name(struct sockaddr_un *addr, const char *name, bool promiscuous, int which)
{
	int name_size;
	const char *runtime_dir;

	runtime_dir = JACK_SOCKET_DIR;

	addr->sun_family = AF_UNIX;
	if (promiscuous) {
		name_size = snprintf(addr->sun_path, sizeof(addr->sun_path),
			     "%s/jack_%s_%d", runtime_dir, name, which) + 1;
	} else {
		name_size = snprintf(addr->sun_path, sizeof(addr->sun_path),
			     "%s/jack_%s_%d_%d", runtime_dir, name, getuid(), which) + 1;
	}

	if (name_size > (int) sizeof(addr->sun_path)) {
		pw_log_error("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
			     runtime_dir, name);
		*addr->sun_path = 0;
		return false;
	}
	return true;
}

static int
notify_client(struct jack_client *client, int ref_num, const char *name, int notify,
	      int sync, const char* message, int value1, int value2)
{
	int size, result = 0;
	char _name[JACK_CLIENT_NAME_SIZE+1];
	char _message[JACK_MESSAGE_SIZE+1];

	if (client->fd == 0)
		return 0;

	if (name == NULL)
		name = client->control->name;

	snprintf(_name, sizeof(_name), "%s", name);
        snprintf(_message, sizeof(_message), "%s", message);

	size = sizeof(int) + sizeof(_name) + 5 * sizeof(int) + sizeof(_message);
	CheckWrite(&size, sizeof(int));
        CheckWrite(_name, sizeof(_name));
	CheckWrite(&ref_num, sizeof(int));
	CheckWrite(&notify, sizeof(int));
	CheckWrite(&value1, sizeof(int));
	CheckWrite(&value2, sizeof(int));
	CheckWrite(&sync, sizeof(int));
        CheckWrite(_message, sizeof(_message));

	if (sync)
		CheckRead(&result, sizeof(int));

	return result;
}

static int
notify_add_client(struct impl *impl, struct jack_client *client, const char *name, int ref_num)
{
	struct jack_server *server = &impl->server;
	int i;

	for (i = 0; i < CLIENT_NUM; i++) {
		struct jack_client *c = server->client_table[i];
		const char *n;

		if (c == NULL || c == client)
			continue;

		n = c->control->name;
		if (notify_client(c, ref_num, name, jack_notify_AddClient, false, "", 0, 0) < 0) {
			pw_log_warn("module-jack %p: can't notify client", impl);
		}
		if (notify_client(client, i, n, jack_notify_AddClient, true, "", 0, 0) < 0) {
			pw_log_error("module-jack %p: can't notify client", impl);
			return -1;
		}
	}
	return 0;
}

void
notify_clients(struct impl *impl, int notify,
	       int sync, const char* message, int value1, int value2)
{
	struct jack_server *server = &impl->server;
	int i;
	for (i = 0; i < CLIENT_NUM; i++) {
		struct jack_client *c = server->client_table[i];

		if (c == NULL)
			continue;

		notify_client(c, i, NULL, notify, sync, message, value1, value2);
	}
}

static int process_messages(struct client *client);

static void client_destroy(void *data)
{
	struct client *this = data;

	pw_loop_destroy_source(pw_core_get_main_loop(this->impl->core), this->source);
	spa_list_remove(&this->link);

	close(this->fd);
}

static int
handle_register_port(struct client *client)
{
	struct impl *impl = client->impl;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	int result = 0;
	int ref_num;
	char name[JACK_PORT_NAME_SIZE + 1];
	char port_type[JACK_PORT_TYPE_SIZE + 1];
	unsigned int flags;
	unsigned int buffer_size;
	static jack_port_id_t port_index = 0;
	jack_port_type_id_t type_id;
	struct jack_client *jc;

	CheckSize(kRegisterPort_size);
	CheckRead(&ref_num, sizeof(int));
	CheckRead(name, sizeof(name));
	CheckRead(port_type, sizeof(port_type));
	CheckRead(&flags, sizeof(unsigned int));
	CheckRead(&buffer_size, sizeof(unsigned int));

	pw_log_debug("protocol-jack %p: kRegisterPort %d %s %s %u %u", impl,
			ref_num, name, port_type, flags, buffer_size);

	type_id = jack_port_get_type_id(port_type);

	if (jack_graph_manager_find_port(mgr, name) != NO_PORT) {
		pw_log_error("protocol-jack %p: port_name %s exists", impl, name);
		result = -1;
		goto reply;
	}

	port_index = jack_graph_manager_allocate_port(mgr, ref_num, name, type_id, flags);
	if (port_index == NO_PORT) {
		pw_log_error("protocol-jack %p: failed to create port name %s", impl, name);
		result = -1;
		goto reply;
	}

	jc = server->client_table[ref_num];
	pw_jack_node_add_port(jc->node,
			      flags & JackPortIsInput ?
				PW_DIRECTION_INPUT :
				PW_DIRECTION_OUTPUT,
			      port_index);

	conn = jack_graph_manager_next_start(mgr);

	if (jack_connection_manager_add_port(conn, (flags & JackPortIsInput) ? true : false,
					     ref_num, port_index) < 0) {
		pw_log_error("protocol-jack %p: failed to add port", impl);
		jack_graph_manager_release_port(mgr, port_index);
		result = -1;
		goto reply_stop;
	}

      reply_stop:
	jack_graph_manager_next_stop(mgr);

	if (jc->control->active)
		notify_clients(impl, jack_notify_PortRegistrationOnCallback, false, "", port_index, 0);

      reply:
	CheckWrite(&result, sizeof(int));
	CheckWrite(&port_index, sizeof(jack_port_id_t));
	return 0;
}

static int
handle_activate_client(struct client *client)
{
	struct impl *impl = client->impl;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	int result = 0;
	int ref_num;
	int is_real_time;

	CheckSize(kActivateClient_size);
	CheckRead(&ref_num, sizeof(int));
	CheckRead(&is_real_time, sizeof(int));

	pw_log_debug("protocol-jack %p: kActivateClient %d %d", client->impl,
			ref_num, is_real_time);

	conn = jack_graph_manager_next_start(mgr);

	jack_connection_manager_direct_connect(conn, server->freewheel_ref_num, ref_num);
	jack_connection_manager_direct_connect(conn, ref_num, server->freewheel_ref_num);

	jack_graph_manager_next_stop(mgr);

	notify_clients(impl, jack_notify_ActivateClient, true, "", 0, 0);

	CheckWrite(&result, sizeof(int));
	return 0;
}

static int
handle_deactivate_client(struct client *client)
{
	struct impl *impl = client->impl;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	int result = 0;
	int ref_num, fw_ref;

	CheckSize(kDeactivateClient_size);
	CheckRead(&ref_num, sizeof(int));

	pw_log_debug("protocol-jack %p: kDeactivateClient %d", client->impl,
			ref_num);

	fw_ref = server->freewheel_ref_num;

	conn = jack_graph_manager_next_start(mgr);

	if (jack_connection_manager_is_direct_connection(conn, fw_ref, ref_num))
		jack_connection_manager_direct_disconnect(conn, fw_ref, ref_num);

	if (jack_connection_manager_is_direct_connection(conn, ref_num, fw_ref))
		jack_connection_manager_direct_disconnect(conn, ref_num, fw_ref);

	jack_graph_manager_next_stop(mgr);

	CheckWrite(&result, sizeof(int));
	return 0;
}

static int
handle_client_check(struct client *client)
{
	char name[JACK_CLIENT_NAME_SIZE+1];
	int protocol;
	int options;
	int UUID;
	int open;
	int result = 0;
	int status;

	CheckSize(kClientCheck_size);
	CheckRead(name, sizeof(name));
	CheckRead(&protocol, sizeof(int));
	CheckRead(&options, sizeof(int));
	CheckRead(&UUID, sizeof(int));
	CheckRead(&open, sizeof(int));

	pw_log_debug("protocol-jack %p: kClientCheck %s %d %d %d %d", client->impl,
			name, protocol, options, UUID, open);

	status = 0;
	if (protocol != JACK_PROTOCOL_VERSION) {
		status |= (JackFailure | JackVersionError);
		pw_log_error("protocol-jack: protocol mismatch (%d vs %d)", protocol, JACK_PROTOCOL_VERSION);
		result = -1;
		goto reply;
	}
	/* TODO check client name and uuid */

      reply:
	CheckWrite(&result, sizeof(int));
	CheckWrite(name, sizeof(name));
	CheckWrite(&status, sizeof(int));

	if (open)
		return process_messages(client);

	return 0;
}

static int
handle_client_open(struct client *client)
{
	struct impl *impl = client->impl;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	int PID, UUID;
	char name[JACK_CLIENT_NAME_SIZE+1];
	int result = -1, ref_num, shared_engine, shared_client, shared_graph;
	struct jack_client *jc;
	const struct ucred *ucred;
	struct sockaddr_un addr;

	CheckSize(kClientOpen_size);
	CheckRead(&PID, sizeof(int));
	CheckRead(&UUID, sizeof(int));
	CheckRead(name, sizeof(name));

	ref_num = jack_server_allocate_ref_num(server);
	if (ref_num == -1) {
		pw_log_error("module-jack %p: can't allocated ref_num", impl);
		goto reply;
	}

	jc = calloc(1,sizeof(struct jack_client));
	jc->owner = client;
	jc->ref_num = ref_num;

	if (jack_synchro_init(&server->synchro_table[ref_num],
			      name,
			      server->engine_control->server_name,
			      0,
			      false,
			      server->promiscuous) < 0) {
		pw_log_error("module-jack %p: can't init synchro", impl);
		goto reply;
	}

	if ((jc->fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		pw_log_error("module-jack %p: can't create socket %s", impl, strerror(errno));
		goto reply;
	}

	if (!init_socket_name(&addr, name, server->promiscuous, 0))
		goto reply;

	if (connect(jc->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		pw_log_error("module-jack %p: can't connect socket %s", impl, strerror(errno));
		goto reply;
	}

	ucred = pw_client_get_ucred(client->client);

	jc->control = jack_client_control_alloc(name, ucred ? ucred->pid : 0, ref_num, -1);
	if (jc->control == NULL) {
		pw_log_error("module-jack %p: can't create control", impl);
		goto reply;
	}

	server->client_table[ref_num] = jc;
	pw_log_debug("module-jack %p: Added client %d \"%s\"", impl, ref_num, name);

	conn = jack_graph_manager_next_start(mgr);
	jack_connection_manager_init_ref_num(conn, ref_num);
	jack_graph_manager_next_stop(mgr);

	jc->node = pw_jack_node_new(impl->core, pw_module_get_global(impl->module),
				    server, ref_num, NULL);

	if (notify_add_client(impl, jc, name, ref_num) < 0) {
		pw_log_error("module-jack %p: can't notify add_client", impl);
		goto reply;
	}

	spa_list_append(&impl->rt.nodes, &jc->node->graph_link);

	shared_engine = impl->server.engine_control->info.index;
	shared_client = jc->control->info.index;
	shared_graph = impl->server.graph_manager->info.index;

	result = 0;

      reply:
	CheckWrite(&result, sizeof(int));
	CheckWrite(&shared_engine, sizeof(int));
	CheckWrite(&shared_client, sizeof(int));
	CheckWrite(&shared_graph, sizeof(int));

	return 0;
}

static int
handle_client_close(struct client *client)
{
	int ref_num;
	CheckSize(kClientClose_size);
	CheckRead(&ref_num, sizeof(int));
	int result = 0;

	CheckWrite(&result, sizeof(int));
	return 0;
}

static int
handle_connect_name_ports(struct client *client)
{
	struct impl *impl = client->impl;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	struct jack_client *jc;
	int ref_num;
	char src[REAL_JACK_PORT_NAME_SIZE+1];
	char dst[REAL_JACK_PORT_NAME_SIZE+1];
	int result = -1, in_ref, out_ref;
	jack_port_id_t src_id, dst_id;
	struct jack_port *src_port, *dst_port;
	struct pw_port *out_port, *in_port;
	struct pw_link *link;

	CheckSize(kConnectNamePorts_size);
	CheckRead(&ref_num, sizeof(int));
	CheckRead(src, sizeof(src));
	CheckRead(dst, sizeof(dst));

	src_id = jack_graph_manager_find_port(mgr, src);
	if (src_id == NO_PORT) {
		pw_log_error("protocol-jack %p: port_name %s does not exist", impl, src);
		goto reply;
	}
	dst_id = jack_graph_manager_find_port(mgr, dst);
	if (dst_id == NO_PORT) {
		pw_log_error("protocol-jack %p: port_name %s does not exist", impl, dst);
		goto reply;
	}

	pw_log_debug("protocol-jack %p: kConnectNamePort %d %s %s %u %u", impl,
			ref_num, src, dst, src_id, dst_id);

	src_port = jack_graph_manager_get_port(mgr, src_id);
	dst_port = jack_graph_manager_get_port(mgr, dst_id);

	if (((src_port->flags & JackPortIsOutput) == 0) ||
	    ((dst_port->flags & JackPortIsInput) == 0)) {
		pw_log_error("protocol-jack %p: ports are not input and output", impl);
		goto reply;
	}

	if (!src_port->in_use || !dst_port->in_use) {
		pw_log_error("protocol-jack %p: ports are not in use", impl);
		goto reply;
	}
	if (src_port->type_id != dst_port->type_id) {
		pw_log_error("protocol-jack %p: ports are not of the same type", impl);
		goto reply;
	}

	conn = jack_graph_manager_next_start(mgr);

	out_ref = jack_connection_manager_get_output_refnum(conn, src_id);
	if (out_ref == -1) {
		pw_log_error("protocol-jack %p: unknown port_id %d", impl, src_id);
		goto reply_stop;
	}
	if ((jc = server->client_table[out_ref]) == NULL) {
		pw_log_error("protocol-jack %p: unknown client %d", impl, out_ref);
		goto reply_stop;
	}
	if (!jc->control->active) {
		pw_log_error("protocol-jack %p: can't connect ports of inactive client", impl);
		goto reply_stop;
	}
	out_port = pw_jack_node_find_port(jc->node, PW_DIRECTION_OUTPUT, src_id);

	in_ref = jack_connection_manager_get_input_refnum(conn, dst_id);
	if (in_ref == -1) {
		pw_log_error("protocol-jack %p: unknown port_id %d", impl, dst_id);
		goto reply_stop;
	}
	if ((jc = server->client_table[in_ref]) == NULL) {
		pw_log_error("protocol-jack %p: unknown client %d", impl, in_ref);
		goto reply_stop;
	}
	if (!jc->control->active) {
		pw_log_error("protocol-jack %p: can't connect ports of inactive client", impl);
		goto reply_stop;
	}
	in_port = pw_jack_node_find_port(jc->node, PW_DIRECTION_INPUT, dst_id);

	if (jack_connection_manager_is_connected(conn, src_id, dst_id)) {
		pw_log_error("protocol-jack %p: ports are already connected", impl);
		goto reply_stop;
	}
	if (jack_connection_manager_connect(conn, src_id, dst_id) < 0) {
		pw_log_error("protocol-jack %p: connection table is full", impl);
		goto reply_stop;
	}
	if (jack_connection_manager_connect(conn, dst_id, src_id) < 0) {
		pw_log_error("protocol-jack %p: connection table is full", impl);
		goto reply_stop;
	}
	if (jack_connection_manager_is_loop_path(conn, src_id, dst_id) < 0)
		jack_connection_manager_inc_feedback_connection(conn, src_id, dst_id);
	else
		jack_connection_manager_inc_direct_connection(conn, src_id, dst_id);

	pw_log_debug("%p %p", out_port, in_port);

	link = pw_link_new(impl->core,
			   pw_module_get_global(impl->module),
			   out_port,
			   in_port,
			   NULL,
			   NULL,
			   NULL,
			   0);
	pw_link_activate(link);

	notify_clients(impl, jack_notify_PortConnectCallback, false, "", src_id, dst_id);

	result = 0;
    reply_stop:
	jack_graph_manager_next_stop(mgr);

    reply:
	CheckWrite(&result, sizeof(int));
	return 0;
}

static int
handle_get_UUID_by_client(struct client *client)
{
	char name[JACK_CLIENT_NAME_SIZE+1];
	char UUID[JACK_UUID_SIZE];
	int result = 0;

	CheckSize(kGetUUIDByClient_size);
	CheckRead(name, sizeof(name));

	CheckWrite(&result, sizeof(int));
	CheckWrite(UUID, sizeof(UUID));

	return 0;
}

static int
process_messages(struct client *client)
{
	struct pw_client *c = client->client;
	int type, res = -1;

	if (read(client->fd, &type, sizeof(enum jack_request_type)) != sizeof(enum jack_request_type)) {
		pw_log_error("protocol-jack %p: failed to read type", client->impl);
		goto error;
	}
	pw_log_info("protocol-jack %p: got type %d", client->impl, type);

	switch(type) {
	case jack_request_RegisterPort:
		res = handle_register_port(client);
		break;
	case jack_request_UnRegisterPort:
		break;
	case jack_request_ConnectPorts:
		break;
	case jack_request_DisconnectPorts:
		break;
	case jack_request_SetTimeBaseClient:
		break;
	case jack_request_ActivateClient:
		res = handle_activate_client(client);
		break;
	case jack_request_DeactivateClient:
		res = handle_deactivate_client(client);
		break;
	case jack_request_DisconnectPort:
		break;
	case jack_request_SetClientCapabilities:
	case jack_request_GetPortConnections:
	case jack_request_GetPortNConnections:
	case jack_request_ReleaseTimebase:
	case jack_request_SetTimebaseCallback:
	case jack_request_SetBufferSize:
	case jack_request_SetFreeWheel:
		break;
	case jack_request_ClientCheck:
		res = handle_client_check(client);
		break;
	case jack_request_ClientOpen:
		res = handle_client_open(client);
		break;
	case jack_request_ClientClose:
		res = handle_client_close(client);
		break;
	case jack_request_ConnectNamePorts:
		res = handle_connect_name_ports(client);
		break;
	case jack_request_DisconnectNamePorts:
		break;
	case jack_request_GetInternalClientName:
	case jack_request_InternalClientHandle:
	case jack_request_InternalClientLoad:
	case jack_request_InternalClientUnload:
	case jack_request_PortRename:
	case jack_request_Notification:
	case jack_request_SessionNotify:
	case jack_request_SessionReply:
	case jack_request_GetClientByUUID:
	case jack_request_ReserveClientName:
		break;
	case jack_request_GetUUIDByClient:
		res = handle_get_UUID_by_client(client);
		break;
	case jack_request_ClientHasSessionCallback:
	case jack_request_ComputeTotalLatencies:
		break;
	default:
		pw_log_error("protocol-jack %p: invalid type %d", client->impl, type);
		goto error;
	}
	if (res != 0)
		goto error;

	return res;

      error:
	pw_log_error("protocol-jack %p: error handling type %d", client->impl, type);
	pw_client_destroy(c);
	return -1;

}

static void
client_busy_changed(void *data, bool busy)
{
	struct client *c = data;
	enum spa_io mask = SPA_IO_ERR | SPA_IO_HUP;

	if (!busy)
		mask |= SPA_IO_IN;

	pw_loop_update_io(pw_core_get_main_loop(c->impl->core), c->source, mask);

	if (!busy)
		process_messages(c);
}

static void
connection_data(void *data, int fd, enum spa_io mask)
{
	struct client *client = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_error("protocol-native %p: got connection error", client->impl);
		pw_client_destroy(client->client);
		return;
	}

	if (mask & SPA_IO_IN)
		process_messages(client);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.destroy = client_destroy,
	.busy_changed = client_busy_changed,
};

static struct client *client_new(struct impl *impl, int fd)
{
	struct client *this;
	struct pw_client *client;
	socklen_t len;
	struct ucred ucred, *ucredp;

	len = sizeof(ucred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0) {
		pw_log_error("no peercred: %m");
		ucredp = NULL;
	} else {
		ucredp = &ucred;
	}

        client = pw_client_new(impl->core, pw_module_get_global(impl->module),
			       ucredp, NULL, sizeof(struct client));
	if (client == NULL)
		goto no_client;

	this = pw_client_get_user_data(client);
	this->impl = impl;
	this->fd = fd;
	this->source = pw_loop_add_io(pw_core_get_main_loop(impl->core),
				      this->fd,
				      SPA_IO_ERR | SPA_IO_HUP, false, connection_data, this);
	if (this->source == NULL)
		goto no_source;

	this->client = client;

	spa_list_insert(impl->client_list.prev, &this->link);

	pw_client_add_listener(client, &this->client_listener, &client_events, this);

	pw_log_debug("module-jack %p: added new client", impl);

	return this;

      no_source:
	free(this);
      no_client:
	return NULL;
}

static void jack_node_pull(void *data)
{
	struct jack_client *jc = data;
	struct impl *impl = jc->data;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct spa_graph_node *n = &jc->node->node->rt.node, *pn;
	struct spa_graph_port *p, *pp;

	jack_graph_manager_try_switch(mgr);

	spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
		if ((pp = p->peer) == NULL || ((pn = pp->node) == NULL))
			continue;
		pn->state = pn->callbacks->process_input(pn->callbacks_data);
	}
}

static void jack_node_push(void *data)
{
	struct jack_client *jc = data;
	struct impl *impl = jc->data;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	int activation;
	struct pw_jack_node *node;
	struct spa_graph_node *n = &jc->node->node->rt.node, *pn;
	struct spa_graph_port *p, *pp;

	conn = jack_graph_manager_get_current(mgr);

	jack_connection_manager_reset(conn, mgr->client_timing);

	activation = jack_connection_manager_get_activation(conn, server->freewheel_ref_num);
	if (activation == 0)
		return;

	pw_log_trace("resume %d", activation);

	spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
		if ((pp = p->peer) == NULL || ((pn = pp->node) == NULL))
			continue;
		pn->state = pn->callbacks->process_output(pn->callbacks_data);
	}

	spa_list_for_each(node, &impl->rt.nodes, graph_link) {
		n = &node->node->rt.node;
		n->state = n->callbacks->process_output(n->callbacks_data);

		spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
			if ((pp = p->peer) == NULL || ((pn = pp->node) == NULL))
				continue;
			pn->state = pn->callbacks->process_input(pn->callbacks_data);
		}

		n->state = n->callbacks->process_input(n->callbacks_data);

		spa_list_for_each(p, &n->ports[SPA_DIRECTION_OUTPUT], link) {
			if ((pp = p->peer) == NULL || ((pn = pp->node) == NULL))
				continue;
			pn->state = pn->callbacks->process_input(pn->callbacks_data);
		}
	}


#if 0
	jack_connection_manager_resume_ref_num(conn,
					       client->control,
					       server->synchro_table,
					       mgr->client_timing);

	if (server->engine_control->sync_mode) {
		pw_log_trace("suspend");
		jack_connection_manager_suspend_ref_num(conn,
						        client->control,
						        server->synchro_table,
						        mgr->client_timing);
	}
#endif
}

static const struct pw_jack_node_events jack_node_events = {
	PW_VERSION_JACK_NODE_EVENTS,
	.pull = jack_node_pull,
	.push = jack_node_push,
};

static int
make_audio_client(struct impl *impl)
{
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	int ref_num;
	struct jack_client *jc;
	jack_port_id_t port_id;

	ref_num = jack_server_allocate_ref_num(server);
	if (ref_num == -1)
		return -1;

	if (jack_synchro_init(&server->synchro_table[ref_num],
			      "system",
			      server->engine_control->server_name,
			      0,
			      false,
			      server->promiscuous) < 0) {
		return -1;
	}

	jc = calloc(1,sizeof(struct jack_client));
	jc->data = impl;
	jc->ref_num = ref_num;
	jc->control = jack_client_control_alloc("system", -1, ref_num, -1);
	jc->control->active = true;

	server->client_table[ref_num] = jc;

	impl->server.engine_control->driver_num++;

	conn = jack_graph_manager_next_start(mgr);

	jack_connection_manager_init_ref_num(conn, ref_num);
	jack_connection_manager_direct_connect(conn, ref_num, ref_num);

	port_id = jack_graph_manager_allocate_port(mgr,
						   ref_num, "system:playback_1", 0,
						   JackPortIsInput |
						   JackPortIsPhysical |
						   JackPortIsTerminal);
	jack_connection_manager_add_port(conn, true, ref_num, port_id);

	port_id = jack_graph_manager_allocate_port(mgr,
						   ref_num, "system:playback_2", 0,
						   JackPortIsInput |
						   JackPortIsPhysical |
						   JackPortIsTerminal);
	jack_connection_manager_add_port(conn, true, ref_num, port_id);

	jack_graph_manager_next_stop(mgr);

	server->audio_ref_num = ref_num;
	server->audio_node = pw_jack_node_new(impl->core, pw_module_get_global(impl->module),
					      server, ref_num, NULL);
	server->audio_node_node = pw_jack_node_get_node(server->audio_node);
	jc->node = server->audio_node;

	pw_jack_node_add_listener(server->audio_node, &jc->node_listener, &jack_node_events, jc);

	pw_log_debug("module-jack %p: Added audio driver %d", impl, ref_num);

	return 0;
}

static int
make_freewheel_client(struct impl *impl)
{
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	int ref_num;
	struct jack_client *jc;

	ref_num = jack_server_allocate_ref_num(server);
	if (ref_num == -1)
		return -1;

	if (jack_synchro_init(&server->synchro_table[ref_num],
			      "freewheel",
			      server->engine_control->server_name,
			      0,
			      false,
			      server->promiscuous) < 0) {
		return -1;
	}

	jc = calloc(1,sizeof(struct jack_client));
	jc->data = impl;
	jc->ref_num = ref_num;
	jc->control = jack_client_control_alloc("freewheel", -1, ref_num, -1);
	jc->control->active = true;

	server->client_table[ref_num] = jc;

	impl->server.engine_control->driver_num++;

	conn = jack_graph_manager_next_start(mgr);

	jack_connection_manager_init_ref_num(conn, ref_num);
	jack_connection_manager_direct_connect(conn, ref_num, ref_num);

	jack_graph_manager_next_stop(mgr);

	server->freewheel_ref_num = ref_num;
	pw_log_debug("module-jack %p: Added freewheel driver %d", impl, ref_num);

	return 0;
}

static bool on_global(void *data, struct pw_global *global)

{
	struct impl *impl = data;
	struct pw_node *node;
	const struct pw_properties *properties;
	const char *str;

	if (pw_global_get_type(global) != impl->t->node)
		return true;

	node = pw_global_get_object(global);

	properties = pw_node_get_properties(node);
	if ((str = pw_properties_get(properties, "media.class")) == NULL)
		return true;

	if (strcmp(str, "Audio/Sink") != 0)
		return true;

	impl->sink_link = pw_link_new(impl->core, pw_module_get_global(impl->module),
		    pw_node_get_free_port(impl->server.audio_node_node, PW_DIRECTION_OUTPUT),
		    pw_node_get_free_port(node, PW_DIRECTION_INPUT),
		    NULL,
		    NULL,
		    NULL,
		    0);
	pw_link_inc_idle(impl->sink_link);

	return false;
}

#if 0
static void on_timeout(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	struct jack_client *client;
	int activation;

	client = server->client_table[server->freewheel_ref_num];

	conn = jack_graph_manager_try_switch(mgr);

	jack_connection_manager_reset(conn, mgr->client_timing);

	activation = jack_connection_manager_get_activation(conn, server->freewheel_ref_num);
	if (activation == 0)
		return;

	pw_log_trace("resume %d", activation);
	jack_connection_manager_resume_ref_num(conn,
					       client->control,
					       server->synchro_table,
					       mgr->client_timing);

	if (server->engine_control->sync_mode) {
		pw_log_trace("suspend");
		jack_connection_manager_suspend_ref_num(conn,
						        client->control,
						        server->synchro_table,
						        mgr->client_timing);
	}
}
#endif

static bool init_nodes(struct impl *impl)
{
	struct pw_core *core = impl->core;
#if 0
	struct timespec timeout, interval;
#endif

	make_audio_client(impl);
	make_freewheel_client(impl);

#if 0
	timeout.tv_sec = 0;
	timeout.tv_nsec = 1;
	interval.tv_sec = 0;
	interval.tv_nsec = 10 * SPA_NSEC_PER_MSEC;

	impl->timer = pw_loop_add_timer(pw_core_get_main_loop(impl->core), on_timeout, impl);
	pw_loop_update_timer(pw_core_get_main_loop(impl->core), impl->timer, &timeout, &interval, false);
#endif

	pw_core_for_each_global(core, on_global, impl);

	return true;
}

static struct socket *create_socket(void)
{
	struct socket *s;

	if ((s = calloc(1, sizeof(struct socket))) == NULL)
		return NULL;

	s->fd = -1;
	return s;
}

static void destroy_socket(struct socket *s)
{
	if (s->source)
		pw_loop_destroy_source(s->loop, s->source);
	if (s->addr.sun_path[0])
		unlink(s->addr.sun_path);
	if (s->fd >= 0)
		close(s->fd);
	if (s->lock_addr[0])
		unlink(s->lock_addr);
	free(s);
}

static void
socket_data(void *data, int fd, enum spa_io mask)
{
	struct impl *impl = data;
	struct client *client;
	struct sockaddr_un name;
	socklen_t length;
	int client_fd;

	length = sizeof(name);
	client_fd = accept4(fd, (struct sockaddr *) &name, &length, SOCK_CLOEXEC);
	if (client_fd < 0) {
		pw_log_error("failed to accept: %m");
		return;
	}

	client = client_new(impl, client_fd);
	if (client == NULL) {
		pw_log_error("failed to create client");
		close(client_fd);
		return;
	}

	pw_loop_update_io(pw_core_get_main_loop(impl->core),
			  client->source, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
}

static bool add_socket(struct impl *impl, struct socket *s)
{
	socklen_t size;

	if ((s->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
		return false;

	size = offsetof(struct sockaddr_un, sun_path) + strlen(s->addr.sun_path);
	if (bind(s->fd, (struct sockaddr *) &s->addr, size) < 0) {
		pw_log_error("bind() failed with error: %m");
		return false;
	}

	if (listen(s->fd, 100) < 0) {
		pw_log_error("listen() failed with error: %m");
		return false;
	}

	s->loop = pw_core_get_main_loop(impl->core);
	s->source = pw_loop_add_io(s->loop, s->fd, SPA_IO_IN, false, socket_data, impl);
	if (s->source == NULL)
		return false;

	spa_list_insert(impl->socket_list.prev, &s->link);

	return true;
}

static int init_server(struct impl *impl, const char *name, bool promiscuous)
{
	struct jack_server *server = &impl->server;
	int i;
	struct socket *s;

	pthread_mutex_init(&server->lock, NULL);

	if (jack_register_server(name, 1) != 0)
		return -1;

	jack_cleanup_shm();

	server->promiscuous = promiscuous;

	/* graph manager */
	server->graph_manager = jack_graph_manager_alloc(2048);

	/* engine control */
	server->engine_control = jack_engine_control_alloc(name);

	for (i = 0; i < CLIENT_NUM; i++)
		server->synchro_table[i] = JACK_SYNCHRO_INIT;

	if (!init_nodes(impl))
		return -1;

	s = create_socket();

	if (!init_socket_name(&s->addr, name, promiscuous, 0))
		goto error;

	if (!add_socket(impl, s))
		goto error;

	return 0;

      error:
	destroy_socket(s);
	return -1;
}


static struct impl *module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct impl *impl;
	const char *name, *str;
	bool promiscuous;

	impl = calloc(1, sizeof(struct impl));
	pw_log_debug("protocol-jack %p: new", impl);

	impl->core = core;
	impl->t = pw_core_get_type(core);
	impl->module = module;
	impl->properties = properties;

	spa_list_init(&impl->socket_list);
	spa_list_init(&impl->client_list);
	spa_list_init(&impl->rt.nodes);

	str = NULL;
	if (impl->properties)
		str = pw_properties_get(impl->properties, "jack.default.server");
	if (str == NULL)
		str = getenv("JACK_DEFAULT_SERVER");

	name = str ? str : JACK_DEFAULT_SERVER_NAME;

	str = NULL;
	if (impl->properties)
		str = pw_properties_get(impl->properties, "jack.promiscuous.server");
	if (str == NULL)
		str = getenv("JACK_PROMISCUOUS_SERVER");

	promiscuous = str ? atoi(str) != 0 : false;

	if (init_server(impl, name, promiscuous) < 0)
		goto error;

	return impl;

      error:
	free(impl);
	return NULL;
}

#if 0
static void module_destroy(struct impl *impl)
{
	struct impl *object, *tmp;

	pw_log_debug("module %p: destroy", impl);

	free(impl);
}
#endif

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	module_init(module, NULL);
	return true;
}