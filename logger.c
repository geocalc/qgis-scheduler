/*
 * logger.c
 *
 *  Created on: 02.02.2016
 *      Author: jh
 */

/*
    Logging mechanics.
    Provides a logging API to push leveled messages.
    Redirects stdout and stderr to log file.

    Copyright (C) 2015,2016  Jörg Habenicht (jh@mwerk.net)

    This file is part of qgis-server-scheduler

    qgis-server-scheduler is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    qgis-server-scheduler is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/


#include "logger.h"

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>

#include "qgis_config.h"
#include "qgis_shutdown_queue.h"

//#undef HAVE_GLIBC_VERSION_2_21	/* used for testing purpose */


static int my_vdprintf(int fd, const char *format, va_list ap) __attribute__ ((__format__ (__printf__, 2, 0)));
static int my_dprintf(int fd, const char *format, ...) __attribute__ ((__format__ (__printf__, 2, 3)));



int my_vdprintf(int fd, const char *format, va_list ap)
{
    int retval;
#ifdef HAVE_GLIBC_VERSION_2_21	/* glibc >= 2.21 */
    retval = vdprintf(fd, format, ap);
#else
    /* workaround for glibc bug which does not handle
     * dprintf() and fork() properly.
     * Solved in glibc-2.21
     * see https://sourceware.org/bugzilla/show_bug.cgi?id=12847
     */
    {
	static const int buffersize = 256;
	char *buffer = malloc(buffersize);
	assert(buffer);
	if ( !buffer )
	{
	    logerror("ERROR: could not allocate memory");
	    qexit(EXIT_FAILURE);
	}

	// copy va list just in case we need to call vsnprintf twice
	va_list aq;
	va_copy(aq, ap);
	retval = vsnprintf(buffer, buffersize, format, aq);
	va_end(aq);
	if (-1 != retval)
	{
	    if (buffersize <= retval)
	    {
		// according to man page the output was truncated
		// try to resize the buffer to correct size and try again
		int newbuffersize = retval+1;
		buffer = realloc(buffer, newbuffersize);
		assert(buffer);
		if ( !buffer )
		{
		    // reallocation failed, old buffer still available, continue printing with old buffer
		    logerror("ERROR: could not reallocate memory");
		    retval = buffersize-1;
		}
		else
		{
		    retval = vsnprintf(buffer, newbuffersize, format, ap);
		    if (newbuffersize <= retval)
		    {
			// according to man page the output was truncated
			retval = newbuffersize-1;
		    }
		}
	    }
	    if (-1 != retval)
	    {
		retval = write(fd, buffer, retval);
	    }
	}
	free(buffer);
    }
#endif

    return retval;
}


int logger_init(void)
{
    return logger_open_logfile();
}


int my_dprintf(int fd, const char *format, ...)
{
    va_list args;
    int retval;

    va_start(args, format);
    retval = my_vdprintf(fd, format, args);
    va_end(args);

    return retval;
}


/* opens the logfile specified by config,
 * redirects stdout and stderr to logfile.
 */
int logger_open_logfile(void)
{
    const char *logfilename = config_get_logfile();
    if (logfilename)
    {

	int retval = open(logfilename, (O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC), (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH));
	if (-1 == retval)
	{
	    logerror("ERROR: can not open log file '%s'", logfilename);
	    qexit(EXIT_FAILURE);
	}
	int logfd = retval;


	/* redirect stdout and stderr to logfile */
	retval = dup3(logfd, STDOUT_FILENO, O_CLOEXEC);
	if (-1 == retval)
	{
	    logerror("ERROR: can not dup to stdout");
	    qexit(EXIT_FAILURE);
	}

	retval = dup3(logfd, STDERR_FILENO, O_CLOEXEC);
	if (-1 == retval)
	{
	    logerror("ERROR: can not dup to stderr");
	    qexit(EXIT_FAILURE);
	}

	retval = close(logfd);
	debug(1, "closed log file fd %d, retval %d, errno %d", logfd, retval, errno);
    }

    return 0;
}


int printlog(const char *format, ...)
{
    assert(format);

    int retval = 0;
    va_list args;

    // prepend some time data before the format string
    static const int timebuffersize = 32;
    struct tm tm;
    char timebuffer[timebuffersize];
    time_t times;
    int strsize = 0;

    time(&times);
    if ((time_t)(-1) == times)
    {
	/* error occured
	 * try to print the text without a time value
	 */
    }
    else
    {
	if (NULL != localtime_r(&times, &tm))
	    strsize = strftime(timebuffer, timebuffersize, "[%F %T] ", &tm);
    }
    /* create a \0 terminated string just in case strftime could not
     * fill the "timebuffer".
     */
    if (0 == strsize)
	timebuffer[0] = '\0';

    {
	strsize += strlen(format);	// add size of 'format'
	strsize += 1;			// add size of "\n"

	// print string
	char strbuffer[strsize+1];
	strcpy(strbuffer, timebuffer);
	strcat(strbuffer, format);
	strcat(strbuffer, "\n");

	va_start(args, format);
	retval = my_vdprintf(STDERR_FILENO, strbuffer, args);
	va_end(args);
    }

    return retval;
}


int mydebug(int level, const char *format, ...)
{
    assert(format);
    assert(level>0);

    int retval = 0;
    int loglevel = config_get_debuglevel();
    if (level <= loglevel)
    {
	va_list args;

	// prepend some time data before the format string
	static const int timebuffersize = 32;
	struct tm tm;
	char timebuffer[timebuffersize];
	time_t times;
	int strsize = 0;

	time(&times);
	if ((time_t)(-1) == times)
	{
	    /* error occured
	     * try to print the text without a time value
	     */
	}
	else
	{
	    if (NULL != localtime_r(&times, &tm))
		strsize = strftime(timebuffer, timebuffersize, "[%F %T]D ", &tm);
	}
	/* create a \0 terminated string just in case strftime could not
	 * fill the "timebuffer".
	 */
	if (0 == strsize)
	    timebuffer[0] = '\0';

	{
	    strsize += strlen(format);	// add size of 'format'
	    strsize += 1;		// add size of "\n"

	    // print string
	    char strbuffer[strsize+1];
	    strcpy(strbuffer, timebuffer);
	    strcat(strbuffer, format);
	    strcat(strbuffer, "\n");

	    va_start(args, format);
	    retval = my_vdprintf(STDERR_FILENO, strbuffer, args);
	    va_end(args);
	}
    }

    return retval;
}


/* Like perror() prints a status and a message to stderr.
 * Unlike perror() this function accepts variable arguments like fprintf() in
 * its argument list. This way you can add more information to the message.
 * If "format" is NULL, it prints the time and the error message from "errno".
 * If "format" is not NULL, it prints the time, the arguments from the format
 * list, an additional colon and space and the error message from "errno".
 */
int logerror(const char *format, ...)
{
    int retval = 0;
    int myerrno = errno;

    if (format)
    {
	va_list args;

	// prepend some time data before the format string
	static const int timebuffersize = 32;
	struct tm tm;
	char timebuffer[timebuffersize];
	time_t times;
	int strsize = 0;

	time(&times);
	if ((time_t)(-1) == times)
	{
	    /* error occured
	     * try to print the text without a time value
	     */
	}
	else
	{
	    if (NULL != localtime_r(&times, &tm))
		strsize = strftime(timebuffer, timebuffersize, "[%F %T] ", &tm);
	}
	/* create a \0 terminated string just in case strftime could not
	 * fill the "timebuffer".
	 */
	if (0 == strsize)
	    timebuffer[0] = '\0';

	{
	    strsize += strlen(format);	// add size of 'format'
	    strsize += 5;		// add size of ": %m\n"

	    // print string
	    char *strbuffer = malloc(strsize+1);
	    if ( !strbuffer )
	    {
		/* malloc failed. exit */
		logerror("ERROR: malloc failed");
		qexit(EXIT_FAILURE);
	    }
	    strcpy(strbuffer, timebuffer);
	    strcat(strbuffer, format);
	    strcat(strbuffer, ": %m\n");

	    va_start(args, format);
	    errno = myerrno;
	    retval = my_vdprintf(STDERR_FILENO, strbuffer, args);
	    va_end(args);

	    free(strbuffer);
	}
    }
    else
    {
	// prepend some time data before the format string
	static const int timebuffersize = 32;
	struct tm tm;
	char timebuffer[timebuffersize];
	time_t times;
	int strsize = 0;

	time(&times);
	if ((time_t)(-1) == times)
	{
	    /* error occured
	     * try to print the text without a time value
	     */
	}
	else
	{
	    if (NULL != localtime_r(&times, &tm))
		strsize = strftime(timebuffer, timebuffersize, "[%F %T] ", &tm);
	}
	/* create a \0 terminated string just in case strftime could not
	 * fill the "timebuffer".
	 */
	if (0 == strsize)
	    timebuffer[0] = '\0';

	// print string
	errno = myerrno;
	retval = my_dprintf(STDERR_FILENO, "%s%m\n", timebuffer);

    }

    return retval;
}



