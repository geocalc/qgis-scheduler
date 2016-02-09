#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([qgis-server-scheduler], [0.7.1], [bugs@mwerk.net])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_CONFIG_SRCDIR([qgis_project.h])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
# FIXME: Replace `main' with a function in `-liniparser':
AC_CHECK_LIB([iniparser], [main])

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
