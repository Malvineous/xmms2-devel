/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003	Peter Alm, Tobias Rundström, Anders Gustafsson
 * 
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 * 
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *                   
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

/** @file 
 * XMMS client lib.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#include <sys/types.h>
#include <pwd.h>

#include "xmmsclientpriv/xmmsclient_list.h"

#include "xmmsclient/xmmsclient.h"
#include "xmmsclientpriv/xmmsclient.h"
#include "xmmsc/xmmsc_idnumbers.h"
#include "xmmsc/xmmsc_stdint.h"
#include "xmmsc/xmmsc_stringport.h"

#define XMMS_MAX_URI_LEN 1024

static void xmmsc_deinit (xmmsc_connection_t *c);

/*
 * Public methods
 */

/**
 * @defgroup XMMSClient XMMSClient
 * @brief This functions are used to connect a client software
 * to the XMMS2 daemon.
 *
 * For proper integration with a client you need to hook the XMMSIPC
 * to your clients mainloop. XMMS2 ships with a couple of default
 * mainloop integrations but can easily be extended to fit your own
 * application.
 *
 * There are three kinds of messages that will be involved in communication
 * with the XMMS2 server. 
 * - Commands: Sent by the client to the server with arguments. Commands will
 * generate a reply to the client.
 * - Broadcasts: Sent by the server to the client if requested. Requesting a 
 * broadcast is done by calling one of the xmmsc_broadcast functions.
 * - Signals: Like broadcasts but they are throttled, the client has to request
 * the next signal when the callback is called. #xmmsc_result_restart is used to
 * "restart" the next signal.
 *
 * Each client command will return a #xmmsc_result_t which holds the command id
 * that will be used to map the result back to the right caller. The #xmmsc_result_t
 * is used to get the result from the server. 
 *
 * @{
 */

/**
 * Initializes a xmmsc_connection_t. Returns %NULL if you
 * runned out of memory.
 *
 * @return a xmmsc_connection_t that should be unreferenced with
 * xmmsc_unref.
 *
 * @sa xmmsc_unref
 */

/* 14:47 <irlanders> isalnum(c) || c == '_' || c == '-'
 */

xmmsc_connection_t *
xmmsc_init (const char *clientname)
{
	xmmsc_connection_t *c;
	int i = 0;
	char j;

	x_api_error_if (!clientname, "with NULL clientname", NULL);

	if (!(c = x_new0 (xmmsc_connection_t, 1))) {
		return NULL;
	}

	while (clientname[i]) {
		j = clientname[i];
		if (!isalnum(j) && j != '_' && j != '-') {
			/* snyggt! */
			free (c);
			x_api_error_if (true, "clientname contains invalid chars, just alphanumeric chars are allowed!", NULL);
		}
		i++;
	}

	if (!(c->clientname = strdup (clientname))) {
		free (c);
		return NULL;
	}

	xmmsc_ref (c);

	return c;
}

static xmmsc_result_t *
xmmsc_send_hello (xmmsc_connection_t *c)
{
	xmms_ipc_msg_t *msg;
	xmmsc_result_t *result;

	msg = xmms_ipc_msg_new (XMMS_IPC_OBJECT_MAIN, XMMS_IPC_CMD_HELLO);
	xmms_ipc_msg_put_int32 (msg, 1); /* PROTOCOL VERSION */
	xmms_ipc_msg_put_string (msg, c->clientname);

	result = xmmsc_send_msg (c, msg);

	return result;
}

/**
 * Connects to the XMMS server.
 * If ipcpath is NULL, it will try to open the default path.
 * 
 * @param c The connection to the server. This must be initialized
 * with #xmmsc_init first.
 * @param ipcpath The IPC path, it's broken down like this: <protocol>://<path>[:<port>].
 * If ipcpath is %NULL it will default to "unix:///tmp/xmms-ipc-<username>"
 * - Protocol could be "tcp" or "unix"
 * - Path is either the UNIX socket, or the ipnumber of the server.
 * - Port is only used when the protocol tcp.
 *
 * @returns TRUE on success and FALSE if some problem
 * occured. call xmmsc_get_last_error to find out.
 *
 * @sa xmmsc_get_last_error
 */

int
xmmsc_connect (xmmsc_connection_t *c, const char *ipcpath)
{
	xmmsc_ipc_t *ipc;
	xmmsc_result_t *result;
	uint32_t i;
	int ret;

	char path[PATH_MAX];

	x_api_error_if (!c, "with a NULL connection", false);

	if (!ipcpath) {
		struct passwd *pwd;

		pwd = getpwuid (getuid ());
		if (!pwd || !pwd->pw_name)
			return false;

		snprintf (path, sizeof(path), "unix:///tmp/xmms-ipc-%s", pwd->pw_name);
	} else {
		snprintf (path, sizeof(path), "%s", ipcpath);
	}

	ipc = xmmsc_ipc_init ();
	
	if (!xmmsc_ipc_connect (ipc, path)) {
		c->error = strdup ("xmms2d is not running.");
		return false;
	}

	c->ipc = ipc;
	result = xmmsc_send_hello (c);
	xmmsc_result_wait (result);
	ret = xmmsc_result_get_uint (result, &i);
	xmmsc_result_unref (result);
	if (!ret) {
		c->error = strdup (xmmsc_ipc_error_get (ipc));
	}

	return ret;
}

/**
 * Set the disconnect callback. It will be called when client will
 * be disconnected.
 */
void
xmmsc_disconnect_callback_set (xmmsc_connection_t *c, void (*callback) (void*), void *userdata)
{
	x_check_conn (c,);
	xmmsc_ipc_disconnect_set (c->ipc, callback, userdata);
}

/**
 * Returns a string that descibes the last error.
 */
char *
xmmsc_get_last_error (xmmsc_connection_t *c)
{
	x_api_error_if (!c, "with a NULL connection", false);
	return c->error;
}

/**
 * Dereference the #xmmsc_connection_t and free
 * the memory when reference count reaches zero.
 */
void
xmmsc_unref (xmmsc_connection_t *c)
{
	x_api_error_if (!c, "with a NULL connection",);
	x_api_error_if (c->ref < 1, "with a freed connection",);

	c->ref--;
	if (c->ref == 0) {
		xmmsc_deinit (c);
	}
}

/**
 * @internal
 */
void
xmmsc_ref (xmmsc_connection_t *c)
{
	x_api_error_if (!c, "with a NULL connection",);

	c->ref++;
}

/**
 * @internal
 * Frees up any resources used by xmmsc_connection_t
 */
static void
xmmsc_deinit (xmmsc_connection_t *c)
{
	xmmsc_ipc_destroy (c->ipc);

	free (c->error);
	free (c->clientname);
	free (c);
}

/**
 * Set locking functions for a connection. Allows simultanous usage of
 * a connection from several threads.
 *
 * @param conn connection
 * @param lock the locking primitive passed to the lock and unlock functions
 * @param lockfunc function called when entering critical region, called with lock as argument.
 * @param unlockfunc funciotn called when leaving critical region.
 */
void
xmmsc_lock_set (xmmsc_connection_t *conn, void *lock, void (*lockfunc)(void *), void (*unlockfunc)(void *))
{
	xmmsc_ipc_lock_set (conn->ipc, lock, lockfunc, unlockfunc);
}

/**
 * Get a list of loaded plugins from the server
 */
xmmsc_result_t *
xmmsc_plugin_list (xmmsc_connection_t *c, uint32_t type)
{
	xmmsc_result_t *res;
	xmms_ipc_msg_t *msg;
	x_check_conn (c, NULL);

	msg = xmms_ipc_msg_new (XMMS_IPC_OBJECT_MAIN, XMMS_IPC_CMD_PLUGIN_LIST);
	xmms_ipc_msg_put_uint32 (msg, type);

	res = xmmsc_send_msg (c, msg);

	return res;
}

/**
 * Get a list of statistics from the server
 */
xmmsc_result_t *
xmmsc_main_stats (xmmsc_connection_t *c)
{
	x_check_conn (c, NULL);

	return xmmsc_send_msg_no_arg (c, XMMS_IPC_OBJECT_MAIN, XMMS_IPC_CMD_STATS);
}


/**
 * Tell the server to quit. This will terminate the server.
 * If you only want to disconnect, use #xmmsc_unref()
 */
xmmsc_result_t *
xmmsc_quit (xmmsc_connection_t *c)
{
	x_check_conn (c, NULL);
	return xmmsc_send_msg_no_arg (c, XMMS_IPC_OBJECT_MAIN, XMMS_IPC_CMD_QUIT);
}

/**
 * Request the quit broadcast.
 * Will be called when the server is terminating.
 */
xmmsc_result_t *
xmmsc_broadcast_quit (xmmsc_connection_t *c)
{
	return xmmsc_send_broadcast_msg (c, XMMS_IPC_SIGNAL_QUIT);
}


/**
 * This function will make a pretty string about the information in
 * the mediainfo hash supplied to it.
 * @param target A allocated char *
 * @param len Length of target
 * @param fmt A format string to use. You can insert items from the hash by
 * using specialformat "${field}".
 * @param table The x_hash_t that you got from xmmsc_result_get_mediainfo
 * @returns The number of chars written to #target
 */

int
xmmsc_entry_format (char *target, int len, const char *fmt, xmmsc_result_t *res)
{
	const char *pos;

	if (!target) {
		return 0;
	}

	if (!fmt) {
		return 0;
	}

	memset (target, 0, len);

	pos = fmt;
	while (strlen (target) + 1 < len) {
		char *next_key, *key, *result = NULL, *end;
		int keylen;

		next_key = strstr (pos, "${");
		if (!next_key) {
			strncat (target, pos, len - strlen (target) - 1);
			break;
		}

		strncat (target, pos, MIN (next_key - pos, len - strlen (target) - 1));
		keylen = strcspn (next_key + 2, "}");
		key = malloc (keylen + 1);

		if (!key) {
			fprintf (stderr, "Unable to allocate %u bytes of memory, OOM?", keylen);
			break;
		}

		memset (key, 0, keylen + 1);
		strncpy (key, next_key + 2, keylen);

		if (strcmp (key, "seconds") == 0) {
			int duration;

			xmmsc_result_get_dict_entry_int32 (res, "duration", &duration);

			if (!duration) {
				strncat (target, "00", len - strlen (target) - 1);
			} else {
				char seconds[10];
				snprintf (seconds, sizeof(seconds), "%02d", (duration/1000)%60);
				strncat (target, seconds, len - strlen (target) - 1);
			}
		} else if (strcmp (key, "minutes") == 0) {
			int duration;

			xmmsc_result_get_dict_entry_int32 (res, "duration", &duration);

			if (!duration) {
				strncat (target, "00", len - strlen (target) - 1);
			} else {
				char minutes[10];
				snprintf (minutes, sizeof(minutes), "%02d", duration/60000);
				strncat (target, minutes, len - strlen (target) - 1);
			}
		} else {
			char tmp[12];

			xmmsc_result_value_type_t type = xmmsc_result_get_dict_entry_type (res, key);
			if (type == XMMSC_RESULT_VALUE_TYPE_STRING) {
				xmmsc_result_get_dict_entry_str (res, key, &result);
			} else if (type == XMMSC_RESULT_VALUE_TYPE_UINT32) {
				uint32_t ui;
				xmmsc_result_get_dict_entry_uint32 (res, key, &ui);
				snprintf (tmp, 12, "%u", ui);
				result = tmp;
			} else if (type == XMMSC_RESULT_VALUE_TYPE_INT32) {
				int32_t i;
				xmmsc_result_get_dict_entry_int32 (res, key, &i);
				snprintf (tmp, 12, "%d", i);
				result = tmp;
			}
				
			if (result)
				strncat (target, result, len - strlen (target) - 1);
		}

		free (key);
		end = strchr (next_key, '}');

		if (!end) {
			break;
		}

		pos = end + 1;
	}

	return strlen (target);
}

/** @} */

/**
 * @internal
 */

static uint32_t
xmmsc_next_id (xmmsc_connection_t *c)
{
	return c->cmd_id++;
}

xmmsc_result_t *
xmmsc_send_broadcast_msg (xmmsc_connection_t *c, uint32_t signalid)
{
	xmms_ipc_msg_t *msg;
	xmmsc_result_t *res;
	
	msg = xmms_ipc_msg_new (XMMS_IPC_OBJECT_SIGNAL, XMMS_IPC_CMD_BROADCAST);
	xmms_ipc_msg_put_uint32 (msg, signalid);
	
	res = xmmsc_send_msg (c, msg);

	xmmsc_result_restartable (res, signalid);

	return res;
}


xmmsc_result_t *
xmmsc_send_signal_msg (xmmsc_connection_t *c, uint32_t signalid)
{
	xmms_ipc_msg_t *msg;
	xmmsc_result_t *res;
	
	msg = xmms_ipc_msg_new (XMMS_IPC_OBJECT_SIGNAL, XMMS_IPC_CMD_SIGNAL);
	xmms_ipc_msg_put_uint32 (msg, signalid);
	
	res = xmmsc_send_msg (c, msg);
	
	xmmsc_result_restartable (res, signalid);

	return res;
}

xmmsc_result_t *
xmmsc_send_msg_no_arg (xmmsc_connection_t *c, int object, int method)
{
	uint32_t cid;
	xmms_ipc_msg_t *msg;

	msg = xmms_ipc_msg_new (object, method);

	cid = xmmsc_next_id (c);
	xmmsc_ipc_msg_write (c->ipc, msg, cid);

	return xmmsc_result_new (c, XMMSC_RESULT_CLASS_DEFAULT, cid);
}

xmmsc_result_t *
xmmsc_send_msg (xmmsc_connection_t *c, xmms_ipc_msg_t *msg)
{
	uint32_t cid;
	xmmsc_result_type_t type;

	cid = xmmsc_next_id (c);

	xmmsc_ipc_msg_write (c->ipc, msg, cid);

	switch (xmms_ipc_msg_get_cmd (msg)) {
		case XMMS_IPC_CMD_SIGNAL:
			type = XMMSC_RESULT_CLASS_SIGNAL;
			break;
		case XMMS_IPC_CMD_BROADCAST:
			type = XMMSC_RESULT_CLASS_BROADCAST;
			break;
		default:
			type = XMMSC_RESULT_CLASS_DEFAULT;
			break;
	}

	return xmmsc_result_new (c, type, cid);
}

/**
 * @defgroup IOFunctions IOFunctions
 * @ingroup XMMSClient
 *
 * @brief Functions for integrating the xmms client library with an
 * existing mainloop. Only to be used if there isn't already
 * integration written (glib, corefoundation or ecore).
 *
 * @{
 */

/**
 * Check for pending output.
 *
 * Used to determine if there is any outgoing data enqueued and the
 * mainloop should flag this socket for writing.
 *
 * @param c connection to check
 * @return 1 output is requested, 0 otherwise
 *
 * @sa xmmsc_io_need_out_callback_set
 */
int
xmmsc_io_want_out (xmmsc_connection_t *c)
{
	x_check_conn (c, -1);

	return xmmsc_ipc_io_out (c->ipc);
}

/**
 * Write pending data.
 *
 * Should be called when the mainloop flags that writing is available
 * on the socket.
 * 
 * @returns 1 if everything is well, 0 if the connection is broken.
 */
int
xmmsc_io_out_handle (xmmsc_connection_t *c)
{
	x_check_conn (c, -1);
	x_api_error_if (!xmmsc_ipc_io_out (c->ipc), "without pending output", -1);
	
	return xmmsc_ipc_io_out_callback (c->ipc);
}

/**
 * Read available data
 *
 * Should be called when the mainloop flags that reading is available
 * on the socket.
 * 
 * @returns 1 if everything is well, 0 if the connection is broken.
 */
int
xmmsc_io_in_handle (xmmsc_connection_t *c)
{
	x_check_conn (c, -1);
	
	return xmmsc_ipc_io_in_callback (c->ipc);
}

/**
 * Retrieve filedescriptor for connection.
 *
 * Gets the underlaying filedescriptor for this connection, or -1 upon
 * error. To be used in a mainloop to do poll/select on. Reading
 * writing should *NOT* be done on this fd, #xmmsc_io_in_handle and
 * #xmmsc_io_out_handle MUST be used to handle reading and writing.
 *
 * @returns underlaying filedescriptor, or -1 on error
 */
int
xmmsc_io_fd_get (xmmsc_connection_t *c)
{
	x_check_conn (c, -1);
	return xmmsc_ipc_fd_get (c->ipc);
}

/**
 * Set callback for enabling/disabling writing.
 *
 * If the mainloop doesn't provide a mechanism to run code before each
 * iteration this function allows registration of a callback to be
 * called when output is needed or not needed any more. The arguments
 * to the callback are flag and userdata; flag is 1 if output is
 * wanted, 0 if not.
 *
 */
void
xmmsc_io_need_out_callback_set (xmmsc_connection_t *c, void (*callback) (int, void*), void *userdata)
{
	x_check_conn (c,);
	xmmsc_ipc_need_out_callback_set (c->ipc, callback, userdata);
}

/**
 * Flag connection as disconnected.
 *
 * To be called when the mainloop signals disconnection of the
 * connection. This is optional, any call to #xmmsc_io_out_handle or
 * #xmmsc_io_in_handle will notice the disconnection and handle it
 * accordingly.
 * 
 */
void
xmmsc_io_disconnect (xmmsc_connection_t *c)
{
	x_check_conn (c,);

	xmmsc_ipc_disconnect (c->ipc);
}

/**
 * @}
 */

void xmms_log_debug (const char *fmt, ...)
{
	char buff[1024];
	va_list ap;

	va_start (ap, fmt);
#ifdef HAVE_VSNPRINTF
	vsnprintf (buff, 1024, fmt, ap);
#else
	vsprintf (buff, fmt, ap);
#endif
	va_end (ap);

	fprintf (stderr, "%s\n", buff);
}

