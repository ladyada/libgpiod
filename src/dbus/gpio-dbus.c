// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * This file is part of libgpiod.
 *
 * Copyright (C) 2018 Bartosz Golaszewski <bartekgola@gmail.com>
 */

#include <gpiod.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gudev/gudev.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

typedef struct ChipCtx {
	struct gpiod_chip *handle;
	guint obj_id;
	GDBusConnection *dbus_conn;
} ChipCtx;

typedef struct MainLoopCtx {
	GMainLoop *loop;
	GUdevClient *udev;
	GDBusConnection *bus;
	guint bus_id;
	GHashTable *chips;
	GDBusNodeInfo *introspect;
} MainLoopCtx;

static const gchar introspect_xml[] =
	"<node>"
	"  <interface name='org.gpiod.Chip'>"
	"    <property name='Name' type='s' access='read' />"
	"    <property name='Label' type='s' access='read' />"
	"    <property name='NumLines' type='u' access='read' />"
	"  </interface>"
	"</node>";

enum {
	CHIP_INTF = 0,
};

static const gchar* const udev_subsystems[] = { "gpio", NULL };

static G_GNUC_NORETURN void die(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	g_logv(NULL, G_LOG_LEVEL_CRITICAL, fmt, va);
	va_end(va);

	exit(EXIT_FAILURE);
}

static const gchar *log_level_to_priority(GLogLevelFlags lvl)
{
	if (lvl & G_LOG_LEVEL_ERROR)
		/*
		 * GLib's ERROR log level is always fatal so translate it
		 * to syslog's EMERG level.
		 */
		return "0";
	else if (lvl & G_LOG_LEVEL_CRITICAL)
		/*
		 * Use GLib's CRITICAL level for error messages. We don't
		 * necessarily want to abort() everytime an error occurred.
		 */
		return "3";
	else if (lvl & G_LOG_LEVEL_WARNING)
		return "4";
	else if (lvl & G_LOG_LEVEL_MESSAGE)
		return "5";
	else if (lvl & G_LOG_LEVEL_INFO)
		return "6";
	else if (lvl & G_LOG_LEVEL_DEBUG)
		return "7";

	/* Default to LOG_NOTICE. */
	return "5";
}

static void handle_log_debug(const gchar *domain, GLogLevelFlags lvl,
			     const gchar *msg, gpointer data G_GNUC_UNUSED)
{
	g_log_structured(domain, lvl, "MESSAGE", msg);
}

static GLogWriterOutput log_write(GLogLevelFlags lvl, const GLogField *fields,
				  gsize n_fields, gpointer data G_GNUC_UNUSED)
{
	const gchar *msg = NULL, *prio;
	const GLogField *field;
	gsize i;

	for (i = 0; i < n_fields; i++) {
		field = &fields[i];

		/* We're only interested in the MESSAGE field. */
		if (!g_strcmp0(field->key, "MESSAGE")) {
			msg = (const gchar *)field->value;
			break;
		}
	}
	if (!msg)
		return G_LOG_WRITER_UNHANDLED;

	prio = log_level_to_priority(lvl);

	g_printerr("<%s>%s\n", prio, msg);

	return G_LOG_WRITER_HANDLED;
}

static gboolean on_sigterm(gpointer data)
{
	MainLoopCtx *ctx = data;

	g_debug("SIGTERM received");

	g_main_loop_quit(ctx->loop);

	return G_SOURCE_REMOVE;
}

static gboolean on_sigint(gpointer data)
{
	MainLoopCtx *ctx = data;

	g_debug("SIGINT received");

	g_main_loop_quit(ctx->loop);

	return G_SOURCE_REMOVE;
}

static gboolean on_sighup(gpointer data G_GNUC_UNUSED)
{
	g_debug("SIGHUB received");

	return G_SOURCE_CONTINUE;
}

static GVariant *
on_chip_property_get(GDBusConnection *connection G_GNUC_UNUSED,
		     const gchar *sender, const gchar *object,
		     const gchar *interface, const gchar *property,
		     GError **error G_GNUC_UNUSED, gpointer user_data)
{
	ChipCtx *chip = user_data;

	g_debug("property get - sender: %s, object: %s, interface: %s, property: %s",
		sender, object, interface, property);

	/* TODO: use a hash table? */
	if (g_strcmp0(property, "Name") == 0)
		return g_variant_new_string(gpiod_chip_name(chip->handle));
	if (g_strcmp0(property, "Label") == 0)
		return g_variant_new_string(gpiod_chip_label(chip->handle));
	if (g_strcmp0(property, "NumLines") == 0)
		return g_variant_new_uint32(gpiod_chip_num_lines(chip->handle));

	return NULL;
}

static const GDBusInterfaceVTable chip_intf_vtable = {
	.get_property = on_chip_property_get,
};

/*
 * We get two uevents per action per gpiochip. One is for the new-style
 * character device, the other for legacy sysfs devices. We are only concerned
 * with the former, which we can tell from the latter by the presence of
 * the device file.
 */
static gboolean is_gpiochip_device(GUdevDevice *dev)
{
	return g_udev_device_get_device_file(dev) != NULL;
}

static void destroy_chip_object(gpointer data)
{
	ChipCtx *chip = data;

	if (chip->handle)
		gpiod_chip_close(chip->handle);

	if (chip->obj_id)
		g_dbus_connection_unregister_object(chip->dbus_conn,
						    chip->obj_id);

	g_free(chip);
}

static void register_chip_object(GUdevDevice *dev, MainLoopCtx *ctx)
{
	const gchar *devname;
	GError *err = NULL;
	gchar *obj_path;
	ChipCtx *chip;
	gboolean ret;

	devname = g_udev_device_get_name(dev);

	g_debug("creating a dbus object for %s", devname);

	chip = g_malloc0(sizeof(ChipCtx));
	chip->dbus_conn = ctx->bus;

	chip->handle = gpiod_chip_open_by_name(devname);
	if (!chip->handle) {
		destroy_chip_object(chip);
		g_warning("error opening GPIO device %s: %s",
			  devname, strerror(errno));
		return;
	}

	obj_path = g_strdup_printf("/org/gpiod/%s", devname);
	g_clear_error(&err);
	chip->obj_id = g_dbus_connection_register_object(ctx->bus, obj_path,
					ctx->introspect->interfaces[CHIP_INTF],
					&chip_intf_vtable, chip, NULL, &err);
	g_free(obj_path);
	if (chip->obj_id == 0) {
		destroy_chip_object(chip);
		g_warning("error registering a dbus object: %s", err->message);
		return;
	}

	ret = g_hash_table_insert(ctx->chips, g_strdup(devname), chip);
	 /*
	  * Key should not exist in the hash table, if it does, then it's
	  * a bug in the program.
	  */
	g_assert_true(ret);
}

static void destroy_chip_dev(GUdevDevice *dev, MainLoopCtx *ctx)
{
	const gchar *devname = g_udev_device_get_name(dev);
	gboolean ret;

	g_debug("removing a dbus object for %s", g_udev_device_get_name(dev));

	ret = g_hash_table_remove(ctx->chips, devname);
	/* It's a programming bug if the key doesn't exist. */
	g_assert_true(ret);
}

static void create_chip_list_cb(gpointer data, gpointer user_data)
{
	MainLoopCtx *ctx = user_data;
	GUdevDevice *dev = data;

	if (is_gpiochip_device(dev))
		register_chip_object(dev, ctx);

	g_object_unref(dev);
}

static void on_uevent(GUdevClient *udev G_GNUC_UNUSED,
		      const gchar *action, GUdevDevice *dev, gpointer data)
{
	MainLoopCtx *ctx = data;

	if (!is_gpiochip_device(dev))
		return;

	g_debug("uevent: %s action on %s device",
		action, g_udev_device_get_name(dev));

	if (g_strcmp0(action, "add") == 0)
		register_chip_object(dev, ctx);
	else if (g_strcmp0(action, "remove") == 0)
		destroy_chip_dev(dev, ctx);
	else
		g_warning("unknown action for uevent: %s", action);
}

static void on_bus_acquired(GDBusConnection *conn,
			    const gchar *name G_GNUC_UNUSED, gpointer data)
{
	MainLoopCtx *ctx = data;

	g_debug("DBus connection acquired");

	ctx->bus = conn;
}

static void on_name_acquired(GDBusConnection *conn G_GNUC_UNUSED,
			     const gchar *name, gpointer data)
{
	MainLoopCtx *ctx = data;
	GList *devs;

	g_debug("DBus name acquired: '%s'", name);

	/* Subscribe for gpio uevents. */
	g_signal_connect(ctx->udev, "uevent", G_CALLBACK(on_uevent), ctx);

	devs = g_udev_client_query_by_subsystem(ctx->udev, "gpio");
	g_list_foreach(devs, create_chip_list_cb, ctx);
	g_list_free(devs);
}

static void on_name_lost(GDBusConnection *conn,
			 const gchar *name, gpointer data G_GNUC_UNUSED)
{
	g_debug("DBus name lost: '%s'", name);

	if (!conn)
		die("unable to make connection to the bus");

	if (g_dbus_connection_is_closed(conn))
		die("connection to the bus closed, dying...");

	die("name '%s' lost on the bus, dying...", name);
}

static void parse_opts(int argc, char **argv)
{
	gboolean rv, opt_debug;
	GError *error = NULL;
	GOptionContext *ctx;
	gchar *summary;

	GOptionEntry opts[] = {
		{
			.long_name		= "debug",
			.short_name		= 'd',
			.flags			= 0,
			.arg			= G_OPTION_ARG_NONE,
			.arg_data		= &opt_debug,
			.description		= "print additional debug messages",
			.arg_description	= NULL,
		},
		{ }
	};

	ctx = g_option_context_new(NULL);

	summary = g_strdup_printf("%s (libgpiod) v%s - dbus daemon for libgpiod",
				  g_get_prgname(), gpiod_version_string());
	g_option_context_set_summary(ctx, summary);
	g_free(summary);

	g_option_context_add_main_entries(ctx, opts, NULL);

	rv = g_option_context_parse(ctx, &argc, &argv, &error);
	if (!rv)
		die("option parsing failed: %s", error->message);

	g_option_context_free(ctx);

	if (opt_debug)
		g_log_set_handler(NULL,
				  G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO,
				  handle_log_debug, NULL);
}

int main(int argc, char **argv)
{
	GError *err = NULL;
	MainLoopCtx ctx;

	memset(&ctx, 0, sizeof(ctx));
	g_set_prgname(program_invocation_short_name);
	g_log_set_writer_func(log_write, NULL, NULL);

	parse_opts(argc, argv);

	g_message("initiating %s", g_get_prgname());

	ctx.loop = g_main_loop_new(NULL, FALSE);

	g_unix_signal_add(SIGTERM, on_sigterm, &ctx);
	g_unix_signal_add(SIGINT, on_sigint, &ctx);
	g_unix_signal_add(SIGHUP, on_sighup, NULL); /* Ignore SIGHUP. */

	g_clear_error(&err);
	ctx.introspect = g_dbus_node_info_new_for_xml(introspect_xml, &err);
	if (!ctx.introspect)
		die("error parsing the introspection xml: %s", err->message);

	ctx.bus_id = g_bus_own_name(G_BUS_TYPE_SYSTEM, "org.gpiod",
				    G_BUS_NAME_OWNER_FLAGS_NONE,
				    on_bus_acquired,
				    on_name_acquired,
				    on_name_lost,
				    &ctx, NULL);

	ctx.udev = g_udev_client_new(udev_subsystems);
	ctx.chips = g_hash_table_new_full(g_str_hash, g_str_equal,
					  g_free, destroy_chip_object);

	g_message("%s started", g_get_prgname());

	g_main_loop_run(ctx.loop);

	g_hash_table_destroy(ctx.chips);
	g_object_unref(ctx.udev);
	g_dbus_node_info_unref(ctx.introspect);
	g_bus_unown_name(ctx.bus_id);
	g_main_loop_unref(ctx.loop);

	g_message("%s exiting cleanly", g_get_prgname());

	return EXIT_SUCCESS;
}
