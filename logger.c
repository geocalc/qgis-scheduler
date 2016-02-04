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


struct thread_logger_args
{
    int in_daemon_mode;
};

static int logfd = -1;
static int pipefd = -1;

static int new_stdout = -1, new_stderr = -1;
pthread_t logger_thread = 0;


int xperror(const char *msg)
{
    int retval = 0;

    if (0 <= new_stderr)
    {
	if (msg)
	    retval = dprintf(new_stderr, "%s: %m", msg);
	else
	    retval = dprintf(new_stderr, "%m");
    }

    return retval;
}


/* Note:
 * perror() writes to stdout, which in this case is redirected to a pipe
 * which gets read() from thread_logger().
 * If we write to stderr inside of the thread, that same thread gets to
 * read() its own error message from the pipe.
 * Substitute the call to stderr so we do write to the logfile
 * and not to the pipe.
 */
static void *thread_logger(void *arg)
{
    assert(arg);
    struct thread_logger_args *tinfo = arg;
    const int in_daemon_mode = tinfo->in_daemon_mode;

    static const int buffersize = 4096;
    static const int timebuffersize = 32;
    char buffer[buffersize];
    struct tm tm;
    char timebuffer[timebuffersize];
    const pthread_t thread_id = pthread_self();
    time_t times;
    int readbytes;

    /* detach myself from the main thread. Doing this to collect resources after
     * this thread ends. Because there is no join() waiting for this thread.
     */
    int retval = pthread_detach(thread_id);
    if (retval)
    {
	errno = retval;
	xperror("error detaching thread");
	exit(EXIT_FAILURE);
    }


    for (;;)
    {
	retval = read(pipefd, buffer, buffersize);
	if (-1 == retval)
	{
	    xperror("can not read from pipe");
	    exit(EXIT_FAILURE);
	}

	readbytes = retval;

	//pid_t pid = getpid();
	time(&times);
	localtime_r(&times, &tm);
	strftime(timebuffer, timebuffersize, "%F %T", &tm);
	if (in_daemon_mode)
	    retval = dprintf(logfd, "[%s] %.*s\n", timebuffer, readbytes, buffer);
	else
	    retval = dprintf(new_stderr, "[%s] %.*s\n", timebuffer, readbytes, buffer);

	if (0 > retval)
	{
	    xperror("can not write to logfile");
	    exit(EXIT_FAILURE);
	}

    }

    /* redirect pipe to stdout and stderr */
    retval = dup2(new_stdout, STDOUT_FILENO);
    if (-1 == retval)
    {
	xperror("can not dup to stdout");
	exit(EXIT_FAILURE);
    }

    retval = dup2(new_stderr, STDERR_FILENO);
    if (-1 == retval)
    {
	xperror("can not dup to stderr");
	exit(EXIT_FAILURE);
    }

    readbytes = sprintf(buffer, "closing logger thread");
    if (in_daemon_mode)
	retval = dprintf(logfd, "[%s] %.*s\n", timebuffer, readbytes, buffer);
    else
	retval = dprintf(new_stderr, "[%s] %.*s\n", timebuffer, readbytes, buffer);

    free(arg);
    return NULL;
}


/* opens the logfile specified by config,
 * redirects stdout and stderr to pipe,
 * create thread to read from pipe
 * and output in new format to logfile.
 */
int logger_init(int in_daemon_mode)
{
    int retval;

    const char *logfilename = config_get_logfile();
    if (logfilename)
    {
	retval = open(logfilename, (O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC), (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH));
	if (-1 == retval)
	{
	    fprintf(stderr, "can not open log file '%s': ", logfilename);
	    perror(NULL);
	    exit(EXIT_FAILURE);
	}
	logfd = retval;
    }

//    /* create internal logging pipe */
//    int pipes[2];
//    retval = pipe2(pipes, O_CLOEXEC);
//    if (-1 == retval)
//    {
//	perror("can not open pipe");
//	exit(EXIT_FAILURE);
//    }
//    pipefd = pipes[0];
//    int pipe_wr = pipes[1];
//
//    /* save old stdout/stderr if not in daemon mode */
//    if ( !in_daemon_mode )
//    {
//	retval = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC);
//	if (-1 == retval)
//	{
//	    perror("can not dup stdout");
//	    exit(EXIT_FAILURE);
//	}
//	new_stdout = retval;
//
//	retval = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC);
//	if (-1 == retval)
//	{
//	    perror("can not dup stderr");
//	    exit(EXIT_FAILURE);
//	}
//	new_stderr = retval;
//    }
//
//    /* create thread to read from pipe and push to logfile or stderr */
//    /* NOTE: aside from the general rule
//     * "malloc() and free() within the same function"
//     * we transfer the responsibility for this memory
//     * to the thread itself.
//     */
//    struct thread_logger_args *targs = malloc(sizeof(*targs));
//    assert(targs);
//    if ( !targs )
//    {
//	perror("could not allocate memory");
//	exit(EXIT_FAILURE);
//    }
//    targs->in_daemon_mode = in_daemon_mode;
//
//    retval = pthread_create(&logger_thread, NULL, thread_logger, targs);
//    if (retval)
//    {
//	errno = retval;
//	perror("error creating thread");
//	exit(EXIT_FAILURE);
//    }
//
//    /* redirect stdout and stderr to pipe */
//    retval = dup3(pipe_wr, STDOUT_FILENO, O_CLOEXEC);
//    if (-1 == retval)
//    {
//	perror("can not dup to stdout");
//	exit(EXIT_FAILURE);
//    }
//
//    retval = dup3(pipe_wr, STDERR_FILENO, O_CLOEXEC);
//    if (-1 == retval)
//    {
//	perror("can not dup to stderr");
//	exit(EXIT_FAILURE);
//    }

    /* redirect stdout and stderr to logfile */
    if (logfilename)
    {
	retval = dup3(logfd, STDOUT_FILENO, O_CLOEXEC);
	if (-1 == retval)
	{
	    perror("can not dup to stdout");
	    exit(EXIT_FAILURE);
	}

	retval = dup3(logfd, STDERR_FILENO, O_CLOEXEC);
	if (-1 == retval)
	{
	    perror("can not dup to stderr");
	    exit(EXIT_FAILURE);
	}
    }

    return 0;
}


void logger_stop(void)
{
//    assert(logger_thread);
//    int retval = pthread_cancel(logger_thread);
//    if (retval)
//    {
//	errno = retval;
//	xperror("error creating thread");
//	exit(EXIT_FAILURE);
//    }
    if (logfd >= 0)
	close(logfd);
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

    time(&times);
    localtime_r(&times, &tm);
    strftime(timebuffer, timebuffersize, "[%F %T] ", &tm);
    int strsize = strlen(timebuffer) + strlen(format);

    {
	// print string
	char strbuffer[strsize+2];
	strcpy(strbuffer, timebuffer);
	strcat(strbuffer, format);
	strcat(strbuffer, "\n");

	va_start(args, format);
	if (-1 != new_stderr)
	    retval = vdprintf(new_stderr, strbuffer, args);
	else
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

	time(&times);
	localtime_r(&times, &tm);
	strftime(timebuffer, timebuffersize, "[%F %T]D ", &tm);
	int strsize = strlen(timebuffer) + strlen(format);

	{
	    // print string
	    char strbuffer[strsize+2];
	    strcpy(strbuffer, timebuffer);
	    strcat(strbuffer, format);
	    strcat(strbuffer, "\n");

	    va_start(args, format);
	    if (-1 != new_stderr)
		retval = vdprintf(new_stderr, strbuffer, args);
	    else
		retval = vdprintf(STDERR_FILENO, strbuffer, args);
	    va_end(args);
	}
    }

    return retval;
}



