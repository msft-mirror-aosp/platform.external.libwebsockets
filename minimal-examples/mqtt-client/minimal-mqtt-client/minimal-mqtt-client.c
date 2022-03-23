/*
 * lws-minimal-mqtt-client
 *
 * Written in 2010-2020 by Andy Green <andy@warmcat.com>
 *                         Sakthi Kannan <saktr@amazon.com>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <assert.h>

enum {
	STATE_SUBSCRIBE,	/* subscribe to the topic */
	STATE_PUBLISH_QOS0,	/* Send the message in QoS0 */
	STATE_WAIT_ACK0,	/* Wait for the synthetic "ack" */
	STATE_PUBLISH_QOS1,	/* Send the message in QoS1 */
	STATE_WAIT_ACK1,	/* Wait for the real ack (or timeout + retry) */

	STATE_TEST_FINISH
};

static int interrupted, bad = 1, do_ssl;

static const lws_retry_bo_t retry = {
	.secs_since_valid_ping		= 20, /* if idle, PINGREQ after secs */
	.secs_since_valid_hangup	= 25, /* hangup if still idle secs */
};

static const lws_mqtt_client_connect_param_t client_connect_param = {
	.client_id			= "lwsMqttClient",
	.keep_alive			= 60,
	.clean_start			= 1,
	.will_param = {
		.topic			= "good/bye",
		.message		= "sign-off",
		.qos			= 0,
		.retain			= 0,
	},
	.username			= "lwsUser",
	.password			= "mySecretPassword",
};

static lws_mqtt_publish_param_t pub_param;

static lws_mqtt_topic_elem_t topics[] = {
	[0] = { .name = "test/topic0", .qos = QOS0 },
	[1] = { .name = "test/topic1", .qos = QOS1 },
};

static lws_mqtt_subscribe_param_t sub_param = {
	.topic				= &topics[0],
	.num_topics			= LWS_ARRAY_SIZE(topics),
};

static const char * const test_string =
	"No one would have believed in the last years of the nineteenth "
	"century that this world was being watched keenly and closely by "
	"intelligences greater than man's and yet as mortal as his own; that as "
	"men busied themselves about their various concerns they were "
	"scrutinised and studied, perhaps almost as narrowly as a man with a "
	"microscope might scrutinise the transient creatures that swarm and "
	"multiply in a drop of water.  With infinite complacency men went to "
	"and fro over this globe about their little affairs, serene in their "
	"assurance of their empire over matter. It is possible that the "
	"infusoria under the microscope do the same.  No one gave a thought to "
	"the older worlds of space as sources of human danger, or thought of "
	"them only to dismiss the idea of life upon them as impossible or "
	"improbable.  It is curious to recall some of the mental habits of "
	"those departed days.  At most terrestrial men fancied there might be "
	"other men upon Mars, perhaps inferior to themselves and ready to "
	"welcome a missionary enterprise. Yet across the gulf of space, minds "
	"that are to our minds as ours are to those of the beasts that perish, "
	"intellects vast and cool and unsympathetic, regarded this earth with "
	"envious eyes, and slowly and surely drew their plans against us.  And "
	"early in the twentieth century came the great disillusionment. ";

/* this reflects the length of the string above */
#define TEST_STRING_LEN 1337

struct pss {
	int		state;
	size_t		pos;
	int		retries;
};

static void
sigint_handler(int sig)
{
	interrupted = 1;
}

static int
connect_client(struct lws_context *context)
{
	struct lws_client_connect_info i;

	memset(&i, 0, sizeof i);

	i.mqtt_cp = &client_connect_param;
	i.address = "localhost";
	i.host = "localhost";
	i.protocol = "mqtt";
	i.context = context;
	i.method = "MQTT";
	i.alpn = "mqtt";
	i.port = 1883;

	if (do_ssl) {
		i.ssl_connection = LCCSCF_USE_SSL;
		i.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED;
		i.port = 8883;
	}

	if (!lws_client_connect_via_info(&i)) {
		lwsl_err("%s: Client Connect Failed\n", __func__);

		return 1;
	}

	return 0;
}

static int
system_notify_cb(lws_state_manager_t *mgr, lws_state_notify_link_t *link,
		 int current, int target)
{
	struct lws_context *context = mgr->parent;

	if (current != LWS_SYSTATE_OPERATIONAL ||
	    target != LWS_SYSTATE_OPERATIONAL)
		return 0;

	/*
	* We delay trying to do the client connection until
	* the protocols have been initialized for each
	* vhost... this happens after we have network and
	* time so we can judge tls cert validity.
	*/

	if (connect_client(context))
		interrupted = 1;

	return 0;
 }


static int
callback_mqtt(struct lws *wsi, enum lws_callback_reasons reason,
	      void *user, void *in, size_t len)
{
	struct pss *pss = (struct pss *)user;
	lws_mqtt_publish_param_t *pub;
	size_t chunk;

	switch (reason) {
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		lwsl_err("%s: CLIENT_CONNECTION_ERROR: %s\n", __func__,
			 in ? (char *)in : "(null)");
		interrupted = 1;
		break;

	case LWS_CALLBACK_MQTT_CLIENT_CLOSED:
		lwsl_user("%s: CLIENT_CLOSED\n", __func__);
		interrupted = 1;
		break;

	case LWS_CALLBACK_MQTT_CLIENT_ESTABLISHED:
		lwsl_user("%s: MQTT_CLIENT_ESTABLISHED\n", __func__);
		lws_callback_on_writable(wsi);

		return 0;

	case LWS_CALLBACK_MQTT_SUBSCRIBED:
		lwsl_user("%s: MQTT_SUBSCRIBED\n", __func__);
		break;

	case LWS_CALLBACK_MQTT_CLIENT_WRITEABLE:
		/*
		 * Extra WRITEABLE may appear here other than ones we asked
		 * for, so we must consult our own state to decide if we want
		 * to make use of the opportunity
		 */

		switch (pss->state) {
		case STATE_SUBSCRIBE:
			lwsl_user("%s: WRITEABLE: Subscribing\n", __func__);

			if (lws_mqtt_client_send_subcribe(wsi, &sub_param)) {
				lwsl_notice("%s: subscribe failed\n", __func__);

				return -1;
			}
			pss->state++;
			break;

		case STATE_PUBLISH_QOS0:
		case STATE_PUBLISH_QOS1:

			lwsl_user("%s: WRITEABLE: Publish\n", __func__);

			pub_param.topic	= "test/topic";
			pub_param.topic_len = (uint16_t)strlen(pub_param.topic);
			pub_param.qos = pss->state == STATE_PUBLISH_QOS0 ? QOS0 : QOS1;
			pub_param.payload_len = TEST_STRING_LEN;

			/* We send the message out 300 bytes or less at at time */

			chunk = 300;

			if (chunk > TEST_STRING_LEN - pss->pos)
				chunk = TEST_STRING_LEN - pss->pos;

			if (lws_mqtt_client_send_publish(wsi, &pub_param,
					test_string + pss->pos, chunk,
					(pss->pos + chunk == TEST_STRING_LEN)))
				return -1;

			pss->pos += chunk;

			if (pss->pos == TEST_STRING_LEN) {
				pss->pos = 0;
				pss->state++;
			}
			break;

		default:
			break;
		}

		return 0;

	case LWS_CALLBACK_MQTT_ACK:
		lwsl_user("%s: MQTT_ACK\n", __func__);
		/*
		 * We can forget about the message we just sent, it's done.
		 *
		 * For our test, that's the indication we can close the wsi.
		 */

		pss->state++;
		if (pss->state != STATE_TEST_FINISH)
			break;

		/* Oh we are done then */

		bad = 0;
		interrupted = 1;
		lws_cancel_service(lws_get_context(wsi));
		break;

	case LWS_CALLBACK_MQTT_RESEND:
		lwsl_user("%s: MQTT_RESEND\n", __func__);
		/*
		 * We must resend the packet ID mentioned in len
		 */
		if (++pss->retries == 3) {
			interrupted = 1;
			break;
		}
		pss->state--;
		pss->pos = 0;
		break;

	case LWS_CALLBACK_MQTT_CLIENT_RX:
		lwsl_user("%s: MQTT_CLIENT_RX\n", __func__);

		pub = (lws_mqtt_publish_param_t *)in;
		assert(pub);

		lwsl_hexdump_notice(pub->topic, pub->topic_len);
		lwsl_hexdump_notice(pub->payload, pub->payload_len);

		return 0;

	default:
		break;
	}

	return 0;
}

static const struct lws_protocols protocols[] = {
	{
		.name			= "mqtt",
		.callback		= callback_mqtt,
		.per_session_data_size	= sizeof(struct pss)
	},
	{ NULL, NULL, 0, 0 }
};

int main(int argc, const char **argv)
{
	lws_state_notify_link_t notifier = { {}, system_notify_cb, "app" };
	lws_state_notify_link_t *na[] = { &notifier, NULL };
	struct lws_context_creation_info info;
	struct lws_context *context;
	int n = 0;

	signal(SIGINT, sigint_handler);
	memset(&info, 0, sizeof info); /* otherwise uninitialized garbage */
	lws_cmdline_option_handle_builtin(argc, argv, &info);

	do_ssl = !!lws_cmdline_option(argc, argv, "-s");
	if (do_ssl)
		info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

	lwsl_user("LWS minimal MQTT client %s [-d<verbosity>][-s]\n",
			do_ssl ? "tls enabled": "unencrypted");

	info.port = CONTEXT_PORT_NO_LISTEN; /* we do not run any server */
	info.protocols = protocols;
	info.register_notifier_list = na;
	info.fd_limit_per_thread = 1 + 1 + 1;
	info.retry_and_idle_policy = &retry;

#if defined(LWS_WITH_MBEDTLS)
	/*
	 * OpenSSL uses the system trust store.  mbedTLS has to be told which
	 * CA to trust explicitly.
	 */
	info.client_ssl_ca_filepath = "./mosq-ca.crt";
#endif

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	/* Event loop */
	while (n >= 0 && !interrupted)
		n = lws_service(context, 0);

	lwsl_user("Completed: %s\n", bad ? "failed" : "OK");
	lws_context_destroy(context);

	return bad;
}
