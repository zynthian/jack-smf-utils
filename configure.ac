#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.71])
AC_INIT([jack-smf-utils],[1.0],[trasz@FreeBSD.org])
AM_INIT_AUTOMAKE([-Wall foreign])
AC_CONFIG_SRCDIR([src/jack-smf-player.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_RANLIB
AM_PROG_AR

# Checks for libraries.
AC_CHECK_LIB([m], [pow])
AC_ARG_WITH([readline],
	    [AS_HELP_STRING([--with-readline],
	    [support fancy command line editing @<:@default=check@:>@])],
	    [],
	    [with_readline=check])

AS_IF([test "x$with_readline" != xno],
      [AC_CHECK_LIB([readline], [main],
		    [AC_SUBST([READLINE_LIBS], ["-lreadline -lncurses"])
		    AC_DEFINE([HAVE_LIBREADLINE], [1], [Define if you have libreadline])],
		    [if test "x$with_readline" != xcheck; then
		     AC_MSG_FAILURE(
				    [--with-readline was given, but test for readline failed])
		     fi
		     ], -lncurses)])

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h stdlib.h string.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_INT8_T

AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_C_VOLATILE

# Checks for library functions.
AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_FUNC_REALLOC
AC_FUNC_STRTOD
AC_CHECK_FUNCS([gettimeofday memset pow strdup strerror strtol])

PKG_CHECK_MODULES(GLIB, glib-2.0 >= 2.3)
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

PKG_CHECK_MODULES(GTHREAD, gthread-2.0 >= 2.3)
AC_SUBST(GTHREAD_CFLAGS)
AC_SUBST(GTHREAD_LIBS)

PKG_CHECK_MODULES(JACK, jack >= 0.102.0)
AC_SUBST(JACK_CFLAGS)
AC_SUBST(JACK_LIBS)

PKG_CHECK_MODULES(JACK_MIDI_NEEDS_NFRAMES, jack < 0.105.00,
	AC_DEFINE(JACK_MIDI_NEEDS_NFRAMES, 1, [whether or not JACK routines need nframes parameter]), true)

AC_ARG_WITH([lash],
	    [AS_HELP_STRING([--with-lash],
	    [support LASH @<:@default=check@:>@])],
	    [],
	    [with_lash=check])

AS_IF([test "x$with_lash" != xno],
      [PKG_CHECK_MODULES(LASH, lash-1.0, AC_DEFINE([HAVE_LASH], [], [Defined if we have LASH support.]),
      [if test "x$with_lash" != xcheck; then
       AC_MSG_FAILURE([--with-lash was given, but LASH was not found])
       fi
       ])])

AC_SUBST(LASH_CFLAGS)
AC_SUBST(LASH_LIBS)

AC_CONFIG_FILES([Makefile libsmf/Makefile man/Makefile src/Makefile])
AC_OUTPUT

