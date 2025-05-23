/*! \file   janus_rabbitmqevh.c
 * \author Piter Konstantinov <pit.here@gmail.com>
 * \copyright GNU General Public License v3
 * \brief  Janus RabbitMQEventHandler plugin
 * \details  This is a trivial RabbitMQ event handler plugin for Janus
 *
 * \ingroup eventhandlers
 * \ref eventhandlers
 */

#include "eventhandler.h"

#include <math.h>

/* Latest RabbitMQ-C library changes the library paths from 0.12.0.0 onwards */
#ifdef HAVE_RABBITMQ_C_AMQP_H
#include <rabbitmq-c/amqp.h>
#include <rabbitmq-c/framing.h>
#include <rabbitmq-c/tcp_socket.h>
#include <rabbitmq-c/ssl_socket.h>
#else
#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>
#include <amqp_ssl_socket.h>
#endif

#include "../debug.h"
#include "../config.h"
#include "../mutex.h"
#include "../utils.h"
#include "../events.h"


/* Plugin information */
#define JANUS_RABBITMQEVH_VERSION			1
#define JANUS_RABBITMQEVH_VERSION_STRING	"0.0.1"
#define JANUS_RABBITMQEVH_DESCRIPTION		"This is a trivial RabbitMQ event handler plugin for Janus."
#define JANUS_RABBITMQEVH_NAME				"JANUS RabbitMQEventHandler plugin"
#define JANUS_RABBITMQEVH_AUTHOR			"Meetecho s.r.l."
#define JANUS_RABBITMQEVH_PACKAGE			"janus.eventhandler.rabbitmqevh"

/* Plugin methods */
janus_eventhandler *create(void);
int janus_rabbitmqevh_init(const char *config_path);
void janus_rabbitmqevh_destroy(void);
int janus_rabbitmqevh_get_api_compatibility(void);
int janus_rabbitmqevh_get_version(void);
const char *janus_rabbitmqevh_get_version_string(void);
const char *janus_rabbitmqevh_get_description(void);
const char *janus_rabbitmqevh_get_name(void);
const char *janus_rabbitmqevh_get_author(void);
const char *janus_rabbitmqevh_get_package(void);
void janus_rabbitmqevh_incoming_event(json_t *event);
json_t *janus_rabbitmqevh_handle_request(json_t *request);

/* Event handler setup */
static janus_eventhandler janus_rabbitmqevh =
	JANUS_EVENTHANDLER_INIT (
		.init = janus_rabbitmqevh_init,
		.destroy = janus_rabbitmqevh_destroy,

		.get_api_compatibility = janus_rabbitmqevh_get_api_compatibility,
		.get_version = janus_rabbitmqevh_get_version,
		.get_version_string = janus_rabbitmqevh_get_version_string,
		.get_description = janus_rabbitmqevh_get_description,
		.get_name = janus_rabbitmqevh_get_name,
		.get_author = janus_rabbitmqevh_get_author,
		.get_package = janus_rabbitmqevh_get_package,

		.incoming_event = janus_rabbitmqevh_incoming_event,
		.handle_request = janus_rabbitmqevh_handle_request,

		.events_mask = JANUS_EVENT_TYPE_NONE
	);

/* Plugin creator */
janus_eventhandler *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_RABBITMQEVH_NAME);
	return &janus_rabbitmqevh;
}


/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;
static GThread *handler_thread;
static GThread *in_thread;
static void *jns_rmqevh_hdlr(void *data);
static void *jns_rmqevh_hrtbt(void *data);
int janus_rabbitmqevh_connect(void);

/* Queue of events to handle */
static GAsyncQueue *events = NULL;
static gboolean group_events = TRUE;
static json_t exit_event;
static void janus_rabbitmqevh_event_free(json_t *event) {
	if(!event || event == &exit_event)
		return;
	json_decref(event);
}

/* JSON serialization options */
static size_t json_format = JSON_INDENT(3) | JSON_PRESERVE_ORDER;

#define JANUS_RABBITMQEVH_EXCHANGE_TYPE "fanout"

/* RabbitMQ session */
static amqp_connection_state_t rmq_conn;
static amqp_channel_t rmq_channel = 0;
static amqp_bytes_t rmq_exchange;

static janus_mutex mutex = JANUS_MUTEX_INITIALIZER;

static char *rmqhost = NULL;
static char *vhost = NULL, *username = NULL, *password = NULL;
static char *ssl_cacert_file = NULL;
static char *ssl_cert_file = NULL;
static char *ssl_key_file = NULL;
static gboolean ssl_enable = FALSE;
static gboolean ssl_verify_peer = FALSE;
static gboolean ssl_verify_hostname = FALSE;
static char *route_key = NULL, *exchange = NULL, *exchange_type = NULL ;
static uint16_t heartbeat = 0;
static uint16_t rmqport = AMQP_PROTOCOL_PORT;
static gboolean declare_outgoing_queue = TRUE;

/* Parameter validation (for tweaking via Admin API) */
static struct janus_json_parameter request_parameters[] = {
	{"request", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter tweak_parameters[] = {
	{"events", JSON_STRING, 0},
	{"grouping", JANUS_JSON_BOOL, 0}
};
/* Error codes (for the tweaking via Admin API */
#define JANUS_RABBITMQEVH_ERROR_INVALID_REQUEST		411
#define JANUS_RABBITMQEVH_ERROR_MISSING_ELEMENT		412
#define JANUS_RABBITMQEVH_ERROR_INVALID_ELEMENT		413
#define JANUS_RABBITMQEVH_ERROR_UNKNOWN_ERROR			499


/* Plugin implementation */
int janus_rabbitmqevh_init(const char *config_path) {
	gboolean success = TRUE;
	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}
	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.jcfg", config_path, JANUS_RABBITMQEVH_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if(config == NULL) {
		JANUS_LOG(LOG_WARN, "Couldn't find .jcfg configuration file (%s), trying .cfg\n", JANUS_RABBITMQEVH_PACKAGE);
		g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_RABBITMQEVH_PACKAGE);
		JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
		config = janus_config_parse(filename);
	}
	if(config != NULL)
		janus_config_print(config);
	janus_config_category *config_general = janus_config_get_create(config, NULL, janus_config_type_category, "general");

	/* Setup the event handler, if required */
	janus_config_item *item = janus_config_get(config, config_general, janus_config_type_item, "enabled");
	if(!item || !item->value || !janus_is_true(item->value)) {
		JANUS_LOG(LOG_WARN, "RabbitMQ event handler disabled\n");
		goto error;
	}

	item = janus_config_get(config, config_general, janus_config_type_item, "json");
	if(item && item->value) {
		/* Check how we need to format/serialize the JSON output */
		if(!strcasecmp(item->value, "indented")) {
			/* Default: indented, we use three spaces for that */
			json_format = JSON_INDENT(3) | JSON_PRESERVE_ORDER;
		} else if(!strcasecmp(item->value, "plain")) {
			/* Not indented and no new lines, but still readable */
			json_format = JSON_INDENT(0) | JSON_PRESERVE_ORDER;
		} else if(!strcasecmp(item->value, "compact")) {
			/* Compact, so no spaces between separators */
			json_format = JSON_COMPACT | JSON_PRESERVE_ORDER;
		} else {
			JANUS_LOG(LOG_WARN, "RabbitMQEventHandler: Unsupported JSON format option '%s', using default (indented)\n", item->value);
			json_format = JSON_INDENT(3) | JSON_PRESERVE_ORDER;
		}
	}

	/* Which events should we subscribe to? */
	item = janus_config_get(config, config_general, janus_config_type_item, "events");
	if(item && item->value)
		janus_events_edit_events_mask(item->value, &janus_rabbitmqevh.events_mask);

	/* Is grouping of events ok? */
	item = janus_config_get(config, config_general, janus_config_type_item, "grouping");
	if(item && item->value)
		group_events = janus_is_true(item->value);

	/* Handle configuration, starting from the server details */
	item = janus_config_get(config, config_general, janus_config_type_item, "host");
	if(item && item->value)
		rmqhost = g_strdup(item->value);
	else
		rmqhost = g_strdup("localhost");

	item = janus_config_get(config, config_general, janus_config_type_item, "port");
	if(item && item->value && janus_string_to_uint16(item->value, &rmqport) < 0) {
		JANUS_LOG(LOG_ERR, "Invalid port (%s), falling back to default\n", item->value);
		rmqport = AMQP_PROTOCOL_PORT;
	}

	/* Credentials and Virtual Host */
	item = janus_config_get(config, config_general, janus_config_type_item, "vhost");
	if(item && item->value)
		vhost = g_strdup(item->value);
	else
		vhost = g_strdup("/");
	item = janus_config_get(config, config_general, janus_config_type_item, "username");
	if(item && item->value)
		username = g_strdup(item->value);
	else
		username = g_strdup("guest");
	item = janus_config_get(config, config_general, janus_config_type_item, "password");
	if(item && item->value)
		password = g_strdup(item->value);
	else
		password = g_strdup("guest");

	item = janus_config_get(config, config_general, janus_config_type_item, "heartbeat");
	if(item && item->value && janus_string_to_uint16(item->value, &heartbeat) < 0) {
		JANUS_LOG(LOG_ERR, "RabbitMQEventHandler: Invalid heartbeat timeout (%s), falling back to default (0, disabling heartbeat)\n", item->value);
		heartbeat = 0;
	}

	/* SSL config*/
	item = janus_config_get(config, config_general, janus_config_type_item, "ssl_enable");
	if(!item || !item->value || !janus_is_true(item->value)) {
		JANUS_LOG(LOG_INFO, "RabbitMQEventHandler: RabbitMQ SSL support disabled\n");
	} else {
		ssl_enable = TRUE;
		item = janus_config_get(config, config_general, janus_config_type_item, "ssl_cacert");
		if(item && item->value)
			ssl_cacert_file = g_strdup(item->value);
		item = janus_config_get(config, config_general, janus_config_type_item, "ssl_cert");
		if(item && item->value)
			ssl_cert_file = g_strdup(item->value);
		item = janus_config_get(config, config_general, janus_config_type_item, "ssl_key");
		if(item && item->value)
			ssl_key_file = g_strdup(item->value);
		item = janus_config_get(config, config_general, janus_config_type_item, "ssl_verify_peer");
		if(item && item->value && janus_is_true(item->value))
			ssl_verify_peer = TRUE;
		item = janus_config_get(config, config_general, janus_config_type_item, "ssl_verify_hostname");
		if(item && item->value && janus_is_true(item->value))
			ssl_verify_hostname = TRUE;
	}

	/* Parse configuration */
	item = janus_config_get(config, config_general, janus_config_type_item, "route_key");
	if(!item || !item->value) {
		JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Missing name of outgoing route_key for RabbitMQ...\n");
		goto error;
	}
	route_key = g_strdup(item->value);

	item = janus_config_get(config, config_general, janus_config_type_item, "exchange_type");
	if(!item || !item->value) {
		exchange_type = (char *)JANUS_RABBITMQEVH_EXCHANGE_TYPE;
	} else {
		exchange_type = g_strdup(item->value);
	}

	/* By default we *DO* declare the outgoing queue */
	item = janus_config_get(config, config_general, janus_config_type_item, "declare_outgoing_queue");
	if(item && item->value && !janus_is_true(item->value)) {
		declare_outgoing_queue = FALSE;
	}

	item = janus_config_get(config, config_general, janus_config_type_item, "exchange");
	if(!item || !item->value) {
		JANUS_LOG(LOG_INFO, "RabbitMQEventHandler: Missing name of outgoing exchange for RabbitMQ, using default\n");
	} else {
		exchange = g_strdup(item->value);
	}
	if (exchange == NULL) {
		JANUS_LOG(LOG_INFO, "RabbitMQEventHandler: enabled, %s:%d (%s) exchange_type:%s\n", rmqhost, rmqport, route_key,exchange_type);
	} else {
		JANUS_LOG(LOG_INFO, "RabbitMQEventHandler: enabled, %s:%d (%s) exch: (%s) exchange_type:%s\n", rmqhost, rmqport, route_key, exchange,exchange_type);
	}

	/* Connect */
	int result = janus_rabbitmqevh_connect();
	if(result < 0) {
		goto error;
	}

	/* Initialize the events queue */
	events = g_async_queue_new_full((GDestroyNotify) janus_rabbitmqevh_event_free);
	g_atomic_int_set(&initialized, 1);

	GError *error = NULL;
	handler_thread = g_thread_try_new("janus rabbitmqevh handler", jns_rmqevh_hdlr, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Got error %d (%s) trying to launch the RabbitMQEventHandler handler thread...\n",
			error->code, error->message ? error->message : "??");
		g_error_free(error);
		goto error;
	}
	if(heartbeat > 0) {
		in_thread = g_thread_try_new("janus rabbitmqevh heartbeat handler", jns_rmqevh_hrtbt, NULL, &error);
		if(error != NULL) {
			g_atomic_int_set(&initialized, 0);
			JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Got error %d (%s) trying to launch the RabbitMQEventHandler heartbeat thread...\n",
				error->code, error->message ? error->message : "??");
			g_error_free(error);
			goto error;
		}
	}

	/* Done */
	JANUS_LOG(LOG_INFO, "Setup of RabbitMQ event handler completed\n");
	goto done;

error:
	/* If we got here, something went wrong */
	success = FALSE;
	g_free(route_key);
	g_free(exchange);
	/* Fall through */
done:
	if(config)
		janus_config_destroy(config);

	if(!success) {
		return -1;
	}
	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_RABBITMQEVH_NAME);
	return 0;
}

int janus_rabbitmqevh_connect(void) {
	rmq_conn = amqp_new_connection();
	amqp_socket_t *socket = NULL;
	int status = AMQP_STATUS_OK;
	JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: Creating RabbitMQ socket...\n");
	if (ssl_enable) {
		socket = amqp_ssl_socket_new(rmq_conn);
		if(socket == NULL) {
			JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Can't connect to RabbitMQ server: error creating socket...\n");
			return -1;
		}

		amqp_ssl_socket_set_verify_peer(socket, ssl_verify_peer);
		amqp_ssl_socket_set_verify_hostname(socket, ssl_verify_hostname);

		if(ssl_cacert_file) {
			status = amqp_ssl_socket_set_cacert(socket, ssl_cacert_file);
			if(status != AMQP_STATUS_OK) {
				JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Can't connect to RabbitMQ server: error setting CA certificate... (%s)\n", amqp_error_string2(status));
				return -1;
			}
		}
		if(ssl_cert_file && ssl_key_file) {
			amqp_ssl_socket_set_key(socket, ssl_cert_file, ssl_key_file);
			if(status != AMQP_STATUS_OK) {
				JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Can't connect to RabbitMQ server: error setting key... (%s)\n", amqp_error_string2(status));
				return -1;
			}
		}
	} else {
		socket = amqp_tcp_socket_new(rmq_conn);
		if(socket == NULL) {
			JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Can't connect to RabbitMQ server: error creating socket...\n");
			return -1;
		}
	}

	JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: Connecting to RabbitMQ server...\n");
	status = amqp_socket_open(socket, rmqhost, rmqport);
	if(status != AMQP_STATUS_OK) {
		JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Can't connect to RabbitMQ server: error opening socket... (%s)\n", amqp_error_string2(status));
		return -1;
	}
	JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: Logging in...\n");
	amqp_rpc_reply_t result = amqp_login(rmq_conn, vhost, 0, 131072, heartbeat, AMQP_SASL_METHOD_PLAIN, username, password);
	if(result.reply_type != AMQP_RESPONSE_NORMAL) {
		JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Can't connect to RabbitMQ server: error logging in... %s, %s\n", amqp_error_string2(result.library_error), amqp_method_name(result.reply.id));
		return -1;
	}

	rmq_channel = 1;
	JANUS_LOG(LOG_VERB, "Opening channel...\n");
	amqp_channel_open(rmq_conn, rmq_channel);
	result = amqp_get_rpc_reply(rmq_conn);
	if(result.reply_type != AMQP_RESPONSE_NORMAL) {
		JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Can't connect to RabbitMQ server: error opening channel... %s, %s\n", amqp_error_string2(result.library_error), amqp_method_name(result.reply.id));
		return -1;
	}
	rmq_exchange = amqp_empty_bytes;
	if(exchange != NULL) {
		JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: Declaring exchange...\n");
		rmq_exchange = amqp_cstring_bytes(exchange);
		amqp_exchange_declare(rmq_conn, rmq_channel, rmq_exchange, amqp_cstring_bytes(exchange_type), 0, 0, 0, 0, amqp_empty_table);
		result = amqp_get_rpc_reply(rmq_conn);
		if(result.reply_type != AMQP_RESPONSE_NORMAL) {
			JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Can't connect to RabbitMQ server: error declaring exchange... %s, %s\n", amqp_error_string2(result.library_error), amqp_method_name(result.reply.id));
			return -1;
		}
	}

	if (declare_outgoing_queue) {
		JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: Declaring outgoing queue... (%s)\n", route_key);
		amqp_queue_declare(rmq_conn, rmq_channel, amqp_cstring_bytes(route_key), 0, 0, 0, 0, amqp_empty_table);
		result = amqp_get_rpc_reply(rmq_conn);
		if(result.reply_type != AMQP_RESPONSE_NORMAL) {
			JANUS_LOG(LOG_FATAL, "RabbitMQEventHandler: Can't connect to RabbitMQ server: error declaring queue... %s, %s\n", amqp_error_string2(result.library_error), amqp_method_name(result.reply.id));
			return -1;
		}
	}

	JANUS_LOG(LOG_INFO, "RabbitMQEventHandler: Connected successfully");

	return 0;
}

void janus_rabbitmqevh_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(events, &exit_event);
	if(handler_thread != NULL) {
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}
	if(in_thread != NULL) {
		g_thread_join(in_thread);
		in_thread = NULL;
	}

	g_async_queue_unref(events);
	events = NULL;

	if(rmq_conn) {
		amqp_destroy_connection(rmq_conn);
	}
	g_free(rmq_exchange.bytes);
	g_free(rmqhost);
	g_free(vhost);
	g_free(username);
	g_free(password);
	g_free(ssl_cacert_file);
	g_free(ssl_cert_file);
	g_free(ssl_key_file);

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_RABBITMQEVH_NAME);
}

int janus_rabbitmqevh_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_EVENTHANDLER_API_VERSION;
}

int janus_rabbitmqevh_get_version(void) {
	return JANUS_RABBITMQEVH_VERSION;
}

const char *janus_rabbitmqevh_get_version_string(void) {
	return JANUS_RABBITMQEVH_VERSION_STRING;
}

const char *janus_rabbitmqevh_get_description(void) {
	return JANUS_RABBITMQEVH_DESCRIPTION;
}

const char *janus_rabbitmqevh_get_name(void) {
	return JANUS_RABBITMQEVH_NAME;
}

const char *janus_rabbitmqevh_get_author(void) {
	return JANUS_RABBITMQEVH_AUTHOR;
}

const char *janus_rabbitmqevh_get_package(void) {
	return JANUS_RABBITMQEVH_PACKAGE;
}

void janus_rabbitmqevh_incoming_event(json_t *event) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		/* Janus is closing or the plugin is */
		return;
	}

	/* Do NOT handle the event here in this callback! Since Janus notifies you right
	 * away when something happens, these events are triggered from working threads and
	 * not some sort of message bus. As such, performing I/O or network operations in
	 * here could dangerously slow Janus down. Let's just reference and enqueue the event,
	 * and handle it in our own thread: the event contains a monotonic time indicator of
	 * when the event actually happened on this machine, so that, if relevant, we can compute
	 * any delay in the actual event processing ourselves. */
	json_incref(event);
	g_async_queue_push(events, event);
}

json_t *janus_rabbitmqevh_handle_request(json_t *request) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}
	/* We can use this requests to apply tweaks to the logic */
	int error_code = 0;
	char error_cause[512];
	JANUS_VALIDATE_JSON_OBJECT(request, request_parameters,
		error_code, error_cause, TRUE,
		JANUS_RABBITMQEVH_ERROR_MISSING_ELEMENT, JANUS_RABBITMQEVH_ERROR_INVALID_ELEMENT);
	if(error_code != 0)
		goto plugin_response;
	/* Get the request */
	const char *request_text = json_string_value(json_object_get(request, "request"));
	if(!strcasecmp(request_text, "tweak")) {
		/* We only support a request to tweak the current settings */
		JANUS_VALIDATE_JSON_OBJECT(request, tweak_parameters,
			error_code, error_cause, TRUE,
			JANUS_RABBITMQEVH_ERROR_MISSING_ELEMENT, JANUS_RABBITMQEVH_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		/* Events */
		if(json_object_get(request, "events"))
			janus_events_edit_events_mask(json_string_value(json_object_get(request, "events")), &janus_rabbitmqevh.events_mask);
		/* Grouping */
		if(json_object_get(request, "grouping"))
			group_events = json_is_true(json_object_get(request, "grouping"));
	} else {
		JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: Unknown request '%s'\n", request_text);
		error_code = JANUS_RABBITMQEVH_ERROR_INVALID_REQUEST;
		g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
	}

plugin_response:
		{
			json_t *response = json_object();
			if(error_code == 0) {
				/* Return a success */
				json_object_set_new(response, "result", json_integer(200));
			} else {
				/* Prepare JSON error event */
				json_object_set_new(response, "error_code", json_integer(error_code));
				json_object_set_new(response, "error", json_string(error_cause));
			}
			return response;
		}
}

/* Thread to handle incoming events */
static void *jns_rmqevh_hdlr(void *data) {
	JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: joining handler thread\n");
	json_t *event = NULL, *output = NULL;
	char *event_text = NULL;
	int count = 0, max = group_events ? 100 : 1;

	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {

		event = g_async_queue_pop(events);
		if(event == &exit_event)
			break;
		count = 0;
		output = NULL;

		while(TRUE) {
			/* Handle event: just for fun, let's see how long it took for us to take care of this */
			json_t *created = json_object_get(event, "timestamp");
			if(created && json_is_integer(created)) {
				gint64 then = json_integer_value(created);
				gint64 now = janus_get_monotonic_time();
				JANUS_LOG(LOG_DBG, "RabbitMQEventHandler: Handled event after %"SCNu64" us\n", now-then);
			}
			if(!group_events) {
				/* We're done here, we just need a single event */
				output = event;
				break;
			}
			/* If we got here, we're grouping */
			if(output == NULL)
				output = json_array();
			json_array_append_new(output, event);
			/* Never group more than a maximum number of events, though, or we might stay here forever */
			count++;
			if(count == max)
				break;
			event = g_async_queue_try_pop(events);
			if(event == NULL || event == &exit_event)
				break;
		}

		if(!g_atomic_int_get(&stopping)) {
			/* Since this a simple plugin, it does the same for all events: so just convert to string... */
			event_text = json_dumps(output, json_format);
			if(event_text == NULL) {
				JANUS_LOG(LOG_WARN, "RabbitMQEventHandler: Failed to stringify event, event lost...\n");
				/* Nothing we can do... get rid of the event */
				json_decref(output);
				output = NULL;
				continue;
			}
			amqp_basic_properties_t props;
			props._flags = 0;
			props._flags |= AMQP_BASIC_CONTENT_TYPE_FLAG;
			props.content_type = amqp_cstring_bytes("application/json");
			amqp_bytes_t message = amqp_cstring_bytes(event_text);
			janus_mutex_lock(&mutex);
			int status = amqp_basic_publish(rmq_conn, rmq_channel, rmq_exchange, amqp_cstring_bytes(route_key), 0, 0, &props, message);
			if(status != AMQP_STATUS_OK) {
				JANUS_LOG(LOG_ERR, "RabbitMQEventHandler: Error publishing... %d, %s\n", status, amqp_error_string2(status));
			}
			janus_mutex_unlock(&mutex);
			free(event_text);
			event_text = NULL;
		}

		/* Done, let's unref the event */
		json_decref(output);
		output = NULL;
	}
	JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: leaving handler thread\n");
	return NULL;
}


/* Thread to handle heartbeats */
static void *jns_rmqevh_hrtbt(void *data) {
	JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: Monitoring RabbitMQ Heartbeat\n");
	int waiting_usec = (heartbeat/2) * 1000000;
	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	amqp_frame_t frame;

	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		janus_mutex_lock(&mutex);
		amqp_maybe_release_buffers(rmq_conn);
		/* Wait for a frame */
		int res = amqp_simple_wait_frame_noblock(rmq_conn, &frame, &timeout);
		janus_mutex_unlock(&mutex);
		if(res != AMQP_STATUS_OK) {
			/* No data */
			if(res == AMQP_STATUS_TIMEOUT || res == AMQP_STATUS_SSL_ERROR) {
				/* Wait half of heartbeat before test again*/
				g_usleep(waiting_usec);
				continue;
			}

			JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: Error on amqp_simple_wait_frame_noblock: %d (%s)\n", res, amqp_error_string2(res));

			if(rmq_conn) {
				amqp_destroy_connection(rmq_conn);
			}
			if(!g_atomic_int_get(&stopping)) {
				JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: Trying to reconnect\n");
				int result = janus_rabbitmqevh_connect();
				if(result < 0) {
					g_usleep(5000000);
				} else {
					g_usleep(waiting_usec);
				}
			}
		} else {
			/* Wait half of heartbeat before test again*/
			g_usleep(waiting_usec);
		}
	}

	JANUS_LOG(LOG_VERB, "RabbitMQEventHandler: Leaving HeartBeat thread\n");
	return NULL;
}
