# SPDX-License-Identifier: LGPL-2.1-or-later

#
# This file is part of libgpiod.
#
# Copyright (C) 2018 Bartosz Golaszewski <bartekgola@gmail.com>
#

AM_CFLAGS = -I$(top_srcdir)/include/ -include $(top_builddir)/config.h
AM_CFLAGS += -Wall -Wextra -g $(GLIB_CFLAGS) $(GIO_CLFAGS) $(GUDEV_CFLAGS)

LDADD = $(top_builddir)/src/lib/libgpiod.la $(GLIB_LIBS) $(GIO_LIBS) $(GUDEV_LIBS)

bin_PROGRAMS = gpio-dbus

gpio_dbus_SOURCES = gpio-dbus.c

EXTRA_DIST = org.gpiod.conf
