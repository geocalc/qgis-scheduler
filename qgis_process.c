/*
 * qgis_process.c
 *
 *  Created on: 06.01.2016
 *      Author: jh
 */

/*
    Database for the QGIS processes.
    Stores file descriptors, status and thread information for the child processes.
    This module does not act on the information, it just stores it to provide for
    future use.

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



#include "qgis_process.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>

#include "logger.h"
#include "timer.h"


/* Default time to wait between sending SIGTERM and SIGKILL */
#define DEFAULT_PROCESS_SIGNAL_TIMEOUT_SEC	10
#define DEFAULT_PROCESS_SIGNAL_TIMEOUT_NANOSEC	0



struct qgis_process_s
{
    enum qgis_process_state_e state;
    pthread_t threadid;		// thread working with this process, no thread = 0
    pid_t pid;			// id of qgis process
    int process_socket_fd;	// fd transferred to the qgis process
    int client_socket_fd;	// fd this scheduler connects to child process
    pthread_mutex_t mutex;	// thread mutex to serialize access
    struct timespec starttime;	// stored start time to measure process runtime
    struct timespec signaltime;	// stored time of last signal send to this process
};


const struct timespec default_signal_timeout =
{
	tv_sec: DEFAULT_PROCESS_SIGNAL_TIMEOUT_SEC,
	tv_nsec: DEFAULT_PROCESS_SIGNAL_TIMEOUT_NANOSEC
};



const char *get_state_str(enum qgis_process_state_e state)
{
    switch (state)
    {
    case PROC_START:
	return "PROC_START";
    case PROC_INIT:
	return "PROC_INIT";
    case PROC_IDLE:
	return "PROC_IDLE";
    case PROC_OPEN_IDLE:
	return "PROC_OPEN_IDLE";
    case PROC_BUSY:
	return "PROC_BUSY";
    case PROC_TERM:
	return "PROC_TERM";
    case PROC_KILL:
	return "PROC_KILL";
    case PROC_EXIT:
	return "PROC_EXIT";
    default:
	assert(0);
    }
    return NULL;
}


struct qgis_process_s *qgis_process_new(pid_t pid, int process_socket_fd)
{
    struct qgis_process_s *proc = calloc(1, sizeof(*proc));
    assert(proc);
    if ( !proc )
    {
	logerror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    proc->pid = pid;
    proc->process_socket_fd = process_socket_fd;
    int retval = pthread_mutex_init(&proc->mutex, NULL);
    if (retval)
    {
	errno = retval;
	logerror("error: pthread_mutex_init");
	exit(EXIT_FAILURE);
    }

    retval = qgis_timer_start(&proc->starttime);
    if (-1 == retval)
    {
	logerror("clock_gettime()");
	exit(EXIT_FAILURE);
    }

    return proc;
}


void qgis_process_delete(struct qgis_process_s *proc)
{
    if (proc)
    {
	// ignore return value, maybe this file is already closed. I don't care
	close(proc->process_socket_fd);
	int retval = pthread_mutex_destroy(&proc->mutex);
	if (retval)
	{
	    errno = retval;
	    logerror("error delete mutex");
	    exit(EXIT_FAILURE);
	}
    }
    free(proc);
}


int qgis_process_set_state_idle(struct qgis_process_s *proc)
{
    assert(proc);
    int retval = -1;
    if (proc)
    {
	switch (proc->state)
	{
	/* transition PROC_START->PROC_IDLE is only allowed
	 * if the started program is not qgis-server
	 */
	case PROC_START:	// fall through
	case PROC_INIT:		// fall through
	case PROC_BUSY:
	    proc->state = PROC_IDLE;
	    // no break
	case PROC_IDLE:
	    proc->threadid = 0;
	    retval = 0;
	    break;

	//case PROC_OPEN_IDLE:
	default:
	    // is this an error?
	    debug(1, "warning: trying to set %s from %s for process %d\n",get_state_str(PROC_IDLE),get_state_str(proc->state),proc->pid);
	    /* do nothing */
	    break;
	}
    }

    return retval;
}


int qgis_process_set_state_busy(struct qgis_process_s *proc, pthread_t thread_id)
{
    assert(proc);
    int retval = -1;
    if (proc)
    {
	switch (proc->state)
	{
	case PROC_IDLE:
	    proc->state = PROC_BUSY;
	    // no break
	case PROC_BUSY:
	    proc->threadid = thread_id;
	    retval = 0;
	    break;

	//case PROC_START:	// fall through
	//case PROC_INIT:	// fall through
	//case PROC_OPEN_IDLE:
	default:
	    // is this an error?
	    debug(1, "warning: trying to set %s from %s for process %d\n",get_state_str(PROC_BUSY),get_state_str(proc->state),proc->pid);
	    /* do nothing */
	    break;
	}
    }

    return retval;
}


int qgis_process_set_state_init(struct qgis_process_s *proc, pthread_t thread_id)
{
    assert(proc);
    int retval = -1;
    if (proc)
    {
	switch (proc->state)
	{
	case PROC_START:
	    proc->state = PROC_INIT;
	    // no break
	case PROC_INIT:
	    proc->threadid = thread_id;
	    retval = 0;
	    break;

	//case PROC_IDLE:
	//case PROC_BUSY:
	//case PROC_OPEN_IDLE:
	default:
	    // is this an error?
	    debug(1, "warning: trying to set %s from %s for process %d\n",get_state_str(PROC_INIT),get_state_str(proc->state),proc->pid);
	    /* do nothing */
	    break;
	}
    }

    return retval;
}


enum qgis_process_state_e qgis_process_get_state(struct qgis_process_s *proc)
{
    assert(proc);
    return proc?proc->state:-1;
}


int qgis_process_get_socketfd(struct qgis_process_s *proc)
{
    assert(proc);
    return proc?proc->process_socket_fd:-1;
}


pthread_mutex_t *qgis_process_get_mutex(struct qgis_process_s *proc)
{
    assert(proc);
    return proc?&proc->mutex:NULL;
}


pid_t qgis_process_get_pid(struct qgis_process_s *proc)
{
    assert(proc);
    return proc?proc->pid:-1;
}


const struct timespec *qgis_process_get_starttime(struct qgis_process_s *proc)
{
    assert(proc);
    return &proc->starttime;
}


const struct timespec *qgis_process_get_signaltime(struct qgis_process_s *proc)
{
    assert(proc);
    return &proc->signaltime;
}


static void qgis_process_set_state_exit(struct qgis_process_s *proc)
{
    printlog("shutdown process %d", proc->pid);
    assert(proc);
    proc->state = PROC_EXIT;
    int retval = close(proc->client_socket_fd);
    debug(1, "closed client socket fd %d, retval %d, errno %d", proc->client_socket_fd, retval, errno);
}


/* send a signal "signum" to the process "proc.pid".
 * if the signal is ok start a timer. if not check if the process still exists
 * (with errno = ESRCH). if it does not, change its status to EXIT. if it does
 * (and signal is not ok) print an error.
 * if "signum" = 0 (just check for existence of proc.pid in kernel space), then
 * if the process does not exist, change its status to EXIT. else do nothing
 * (no timer start and no error message).
 */
static void qgis_process_send_signal(struct qgis_process_s *proc, int signum)
{
    assert(proc);
    assert(proc->pid > 0);
    assert(signum >= 0);

    int retval = kill(proc->pid, signum);
    if (retval)
    {
	if (ESRCH == errno)
	{
	    /* process does not exist anymore.
	     * change state to PROC_EXIT.
	     */
	    qgis_process_set_state_exit(proc);
	}
	else if (signum)
	{
	    logerror("error: %s:%d kill pid %d", __FUNCTION__, __LINE__, proc->pid);
	    exit(EXIT_FAILURE);
	}
    }
    else if (signum)
    {
	retval = qgis_timer_start(&proc->signaltime);
	if (-1 == retval)
	{
	    logerror("clock_gettime()");
	    exit(EXIT_FAILURE);
	}
    }

}

void qgis_process_signal_shutdown(struct qgis_process_s *proc)
{
    /* If the process is not in shutdown transition (PROC_TERM, PROC_KILL,
     * PROC_EXIT) then signal SIGTERM, change state to PROC_TERM and store
     * the time of signal in signaltime.
     * If the process is in state PROC_TERM and the signal has been send
     * n seconds ago (default n=10), then signal SIGKILL, change state to
     * PROC_KILL and store the time of signal in signaltime.
     * If the process is in state PROC_KILL and the signal has been send
     * n seconds ago (default n=10), then change state to PROC_EXIT. (This
     * means we can not help that stubborn process which does not react to
     * signal(SIGKILL))
     */
    assert(proc);
    int retval;

    switch(proc->state)
    {
    case PROC_START:
    case PROC_INIT:
    case PROC_IDLE:
    case PROC_OPEN_IDLE:
    case PROC_BUSY:
	/* note: set status before call to function because
	 * qgis_process_send_signal() may change state to EXIT
	 */
	proc->state = PROC_TERM;
	qgis_process_send_signal(proc, SIGTERM);
	break;

    case PROC_TERM:
    {
	struct timespec tm;
	retval = qgis_timer_sub(&proc->signaltime, &tm);
	if (-1 == retval)
	{
	    logerror("clock_gettime()");
	    exit(EXIT_FAILURE);
	}

	if (qgis_timer_isgreaterthan(&tm, &default_signal_timeout))
	{
	    /* the process still exists after n seconds timeout.
	     * send a SIGKILL and change state to PROC_KILL.
	     */
	    /* note: set status before call to function because
	     * qgis_process_send_signal() may change state to EXIT
	     */
	    proc->state = PROC_KILL;
	    qgis_process_send_signal(proc, SIGKILL);
	}
	else
	{
	    /* the process has received a signal just some time fractions ago.
	     * check if it still exists. if not change its status to EXIT.
	     */
	    qgis_process_send_signal(proc, 0);
	}
	break;
    }
    case PROC_KILL:
    {
	struct timespec tm;
	retval = qgis_timer_sub(&proc->signaltime, &tm);
	if (-1 == retval)
	{
	    logerror("clock_gettime()");
	    exit(EXIT_FAILURE);
	}

	if (qgis_timer_isgreaterthan(&tm, &default_signal_timeout))
	{
	    /* the process still exists after n seconds timeout?!?
	     * change state to PROC_EXIT. We can not help it anymore.
	     */
	    qgis_process_set_state_exit(proc);
	}
	else
	{
	    /* the process has received a signal just some time fractions ago.
	     * check if it still exists. if not change its status to EXIT.
	     */
	    qgis_process_send_signal(proc, 0);
	}
	break;
    }
    case PROC_EXIT:
	/* process is dead already. nothing to be done */
	break;

    default:
	printlog("error: %s:%d unknown state %d", __FUNCTION__, __LINE__, proc->state);
    }


}



