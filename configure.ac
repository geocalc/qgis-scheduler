#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([qgis-server-scheduler], [0.8.1], [bugs@mwerk.net])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_CONFIG_SRCDIR([qgis_project.h])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
# FIXME: Replace `main' with a function in `-liniparser':
AC_CHECK_LIB([iniparser], [main], [], AC_MSG_ERROR([can not find lib iniparser from https://github.com/ndevilla/iniparser]))

# check for glibc version >= 2.21.
# check for presence of glibc version >= 2.21. Previous versions cause this
# program to receive SIGSEGV.
# see https://sourceware.org/bugzilla/show_bug.cgi?id=12847
#
# glibc >= 2.21 has removed the function "sigvec".
# note: This behaviour only appreas with glibc <2.21. so better check
# with test program and  gnu_get_libc_version (). If that program fails
# then there is no glibc. If it succeeds, we can check the version.
# #include <stdio.h>
# #include <gnu/libc-version.h>
# int main (void) { puts (gnu_get_libc_version ()); return 0; }
AC_CHECK_LIB([c], [sigvec], [] , AC_DEFINE([HAVE_GLIBC_VERSION_2_21], [1], [Define to 1 if version of glibc >= 2.21]))

# check for pthreads
# see https://airtower.wordpress.com/2010/06/14/autotools-and-pthreads/
# and http://www.gnu.org/software/autoconf-archive/ax_pthread.html
# note gentoo: using sys-devel/autoconf-archive
AX_PTHREAD
LIBS="$PTHREAD_LIBS $LIBS"
CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
CC="$PTHREAD_CC"

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h limits.h netdb.h stddef.h stdint.h stdlib.h string.h sys/socket.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T

# Checks for library functions.
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_CHECK_FUNCS([clock_gettime dup2 localtime_r memset regcomp select socket strdup strrchr])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
