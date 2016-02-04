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

#define _GNU_SOURCE

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




/* opens the logfile specified by config,
 * redirects stdout and stderr to logfile.
 */
int logger_init(void)
{
    const char *logfilename = config_get_logfile();
    if (logfilename)
    {

	int retval = open(logfilename, (O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC), (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH));
	if (-1 == retval)
	{
	    fprintf(stderr, "can not open log file '%s': ", logfilename);
	    logerror(NULL);
	    exit(EXIT_FAILURE);
	}
	int logfd = retval;


	/* redirect stdout and stderr to logfile */
	retval = dup3(logfd, STDOUT_FILENO, O_CLOEXEC);
	if (-1 == retval)
	{
	    logerror("can not dup to stdout");
	    exit(EXIT_FAILURE);
	}

	retval = dup3(logfd, STDERR_FILENO, O_CLOEXEC);
	if (-1 == retval)
	{
	    logerror("can not dup to stderr");
	    exit(EXIT_FAILURE);
	}

	close(logfd);
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
	strsize +=1;			// add size of "\n"

	// print string
	char strbuffer[strsize+1];
	strcpy(strbuffer, timebuffer);
	strcat(strbuffer, format);
	strcat(strbuffer, "\n");

	va_start(args, format);
	retval = vdprintf(STDERR_FILENO, strbuffer, args);
	va_end(args);
    }

    return retval;
}


int debug(int level, const char *format, ...)
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
	    retval = vdprintf(STDERR_FILENO, strbuffer, args);
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
	    char strbuffer[strsize+1];
	    strcpy(strbuffer, timebuffer);
	    strcat(strbuffer, format);
	    strcat(strbuffer, ": %m\n");

	    va_start(args, format);
	    errno = myerrno;
	    retval = vdprintf(STDERR_FILENO, strbuffer, args);
	    va_end(args);
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

	{
	    strsize += 3;		// add size of "%m\n"

	    // print string
	    char strbuffer[strsize+1];
	    strcpy(strbuffer, timebuffer);
	    strcat(strbuffer, "%m\n");

	    errno = myerrno;
	    retval = dprintf(STDERR_FILENO, strbuffer);
	}

    }

    return retval;
}



