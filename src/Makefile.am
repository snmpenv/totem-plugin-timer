LIBTOOLFLAGS=--silent

plugin_ldflags = \
  -export-dynamic \
  -no-undefined \
  -avoid-version \
  -module

totempluginsdir=$(shell @PKG_CONFIG@ --variable=pluginsdir totem)
libdir=${totempluginsdir}/@PACKAGE@

lib_LTLIBRARIES=libtimer.la

libtimer_la_SOURCES=timer.c
libtimer_la_CFLAGS=$(DEPS_CFLAGS) -Wall
libtimer_la_LDFLAGS=$(DEPS_LIBS)$(plugin_ldflags) -version-info 1:0:0

timer_plugindir=$(libdir)
timer_plugin_DATA=timer.plugin

uninstall-hook:
	rm -df "$(DESTDIR)$(libdir)"
