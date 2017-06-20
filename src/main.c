/*
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2015, CESAR. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the CESAR nor the
 *      names of its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL CESAR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <sys/inotify.h>
#include <json-c/json.h>

#include <hal/linux_log.h>
#include "settings.h"
#include "manager.h"

#define BUF_LEN (sizeof(struct inotify_event))

static GMainLoop *main_loop;

static struct settings settings;

static const char *opt_cfg = "/etc/knot/gatewayConfig.json";
/*
 * Settings provided by command line have higher
 * priority than values read from config file.
 */
static unsigned int opt_port = 0;
static const char *opt_host = NULL;
/* Default is websockets */
static const char *opt_proto = "ws";
static const char *opt_tty = NULL;
static gboolean opt_detach = TRUE;
static gboolean opt_disable_nobody = TRUE;

static void sig_term(int sig)
{
	g_main_loop_quit(main_loop);
}

static GOptionEntry options[] = {
	{ "config", 'c', 0, G_OPTION_ARG_STRING, &opt_cfg,
					"configuration file path", NULL },
	{ "host", 'h', 0, G_OPTION_ARG_STRING, &opt_host,
					"host", "Cloud server URL" },
	{ "port", 'p', 0, G_OPTION_ARG_INT, &opt_port,
					"port", "Cloud server port" },
	{ "proto", 'P', 0, G_OPTION_ARG_STRING, &opt_proto,
					"protocol", "eg: http or ws" },
	{ "tty", 't', 0, G_OPTION_ARG_STRING, &opt_tty,
					"TTY", "eg: /dev/ttyUSB0" },
	{ "nodetach", 'n', G_OPTION_FLAG_REVERSE,
					G_OPTION_ARG_NONE, &opt_detach,
					"Logging in foreground" },
	{ "disable-nobody", 'b', G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE,
					&opt_disable_nobody, "disable-nobody",
					"Disable set uid to nobody" },

	{ NULL },
};

static int parse_config(json_object *jobj, struct settings *settings)
{
	const char *uuid, *tmp;
	json_object *obj_cloud, *obj_tmp;
	int err = -EINVAL;

	if (!json_object_object_get_ex(jobj, "cloud", &obj_cloud))
		goto done;

	if (!json_object_object_get_ex(obj_cloud, "uuid", &obj_tmp))
		goto done;

	/* UUID is mandatory */
	uuid = json_object_get_string(obj_tmp);
	if (!uuid)
		goto done;

	if (settings->host == NULL) {
		if (!json_object_object_get_ex(obj_cloud, "serverName",
								 &obj_tmp))
			goto done;

		tmp = json_object_get_string(obj_tmp);
		settings->host = g_strdup(tmp);
	}

	if (settings->port == 0) {
		if (!json_object_object_get_ex(obj_cloud, "port", &obj_tmp))
			goto done;

		settings->port = json_object_get_int(obj_tmp);
	}

	settings->uuid = g_strdup(uuid);

	err = 0; /* Success */
done:
	return err;
}

static gboolean inotify_cb(GIOChannel *gio, GIOCondition condition,
								gpointer data)
{
	int inotifyFD = g_io_channel_unix_get_fd(gio);
	char buf[BUF_LEN];
	ssize_t numRead;
	const struct inotify_event *event;

	numRead = read(inotifyFD, buf, BUF_LEN);
	if (numRead == -1) {
		hal_log_error("Error read from inotify fd");
		return FALSE;
	}

	hal_log_info("Read %ld bytes from inotify fd", (long) numRead);

	/* Process the events in buffer returned by read() */

	event = (struct inotify_event *) buf;
	if (event->mask & IN_MODIFY)
		g_main_loop_quit(main_loop);

	return TRUE;
}

int main(int argc, char *argv[])
{
	GOptionContext *context;
	GError *gerr = NULL;
	int err;
	json_object *jobj;
	GIOChannel *inotify_io;
	int inotifyFD, wd;
	guint watch_id;

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (!g_option_context_parse(context, &argc, &argv, &gerr)) {
		g_printerr("Invalid arguments: %s\n", gerr->message);
		g_error_free(gerr);
		g_option_context_free(context);
		hal_log_close();
		return EXIT_FAILURE;
	}

	g_option_context_free(context);

	if (!opt_cfg) {
		g_printerr("Missing KNOT configuration file!\n");
		hal_log_close();
		return EXIT_FAILURE;
	}

	memset(&settings, 0, sizeof(settings));
	settings.proto = opt_proto;
	settings.tty = opt_tty;
	/*
	 * Command line options (host and port) have higher priority
	 * than values read from config file. UUID should
	 * not be read from command line due security reason.
	 */

	settings.port = opt_port;

	/* Load data from config file */
	jobj = json_object_from_file(opt_cfg);
	if (!jobj)
		goto failure;

	err = parse_config(jobj, &settings);
	/* Free mem derived from jobj*/
	json_object_put(jobj);
	if (err < 0)
		goto failure;

	if (opt_host) {
		g_free(settings.host);
		settings.host = g_strdup(opt_host);
	}

	hal_log_init("knotd", opt_detach);
	hal_log_info("KNOT Gateway");

	err = manager_start(&settings);
	if (err < 0) {
		hal_log_error("start(): %s (%d)", strerror(-err), -err);
		goto failure;
	}

	/* Set user id to nobody */
	if (opt_disable_nobody) {
		err = setuid(65534);
		if (err != 0) {
			manager_stop();
			hal_log_error("Set uid to nobody failed.  " \
				"%s(%d). Exiting ...", strerror(errno), errno);
			goto failure;
		}
	}

	/*
	 * TODO: implement a robust & clean way to reload settings
	 * instead of force quitting when configuration file changes.
	 */

	/* Starting inotify */
	inotifyFD = inotify_init();

	wd = inotify_add_watch(inotifyFD, opt_cfg, IN_MODIFY);
	if (wd == -1) {
		manager_stop();
		close(inotifyFD);
		hal_log_error("inotify_add_watch(): %s", opt_cfg);
		goto failure;
	}
	/* Setting gio channel to watch inotify fd */
	inotify_io = g_io_channel_unix_new(inotifyFD);
	watch_id = g_io_add_watch(inotify_io, G_IO_IN, inotify_cb, NULL);
	g_io_channel_set_close_on_unref(inotify_io, TRUE);
	g_io_channel_unref(inotify_io);


	signal(SIGTERM, sig_term);
	signal(SIGINT, sig_term);
	signal(SIGPIPE, SIG_IGN);

	main_loop = g_main_loop_new(NULL, FALSE);

	if (opt_detach) {
		if (daemon(0, 0)) {
			hal_log_error("Can't start daemon!");
			goto failure;
		}
	}

	g_main_loop_run(main_loop);

	g_main_loop_unref(main_loop);

	/* inotify cleanup */
	g_source_remove(watch_id);
	inotify_rm_watch(inotifyFD, wd);

	manager_stop();
	g_free(settings.host);
	g_free(settings.uuid);

	hal_log_info("Exiting");
	hal_log_close();

	return EXIT_SUCCESS;

failure:
	g_free(settings.host);
	g_free(settings.uuid);

	hal_log_close();
	return EXIT_FAILURE;
}
