// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * This file is part of libgpiod.
 *
 * Copyright (C) 2018 Bartosz Golaszewski <bartekgola@gmail.com>
 */

#include "gpiodaemon.h"

#include <gudev/gudev.h>

struct _GPIODaemon {
	GObject parent;

	GDBusConnection *conn;
	GUdevClient *udev;
};

struct _GPIODaemonClass {
	GObjectClass parent;
};

typedef struct _GPIODaemonClass GPIODaemonClass;

G_DEFINE_TYPE(GPIODaemon, gpio_daemon, G_TYPE_OBJECT);

static void gpio_daemon_init(GPIODaemon *daemon G_GNUC_UNUSED)
{

}

GPIODaemon *gpio_daemon_new(void)
{
	return GPIO_DAEMON(g_object_new(GPIO_DAEMON_TYPE, NULL));
}

static void gpio_daemon_class_init(GPIODaemonClass *_class G_GNUC_UNUSED)
{

}
