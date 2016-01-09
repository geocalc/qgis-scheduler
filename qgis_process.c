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


struct qgis_process_s
{
    enum qgis_process_state_e state;
    pthread_t threadid;		// thread working with this process, no thread = 0
    pid_t pid;			// id of qgis process
    int process_socket_fd;	// fd transferred to the qgis process
    int client_socket_fd;	// fd this scheduler connects to child process
    pthread_mutex_t mutex;	// thread mutex to serialize access
};




static const char *get_state_str(enum qgis_process_state_e state)
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
    }
    return NULL;
}


struct qgis_process_s *qgis_process_new(pid_t pid, int process_socket_fd)
{
    struct qgis_process_s *proc = calloc(1, sizeof(*proc));
    assert(proc);
    if (proc)
    {
	proc->pid = pid;
	proc->process_socket_fd = process_socket_fd;
	int retval = pthread_mutex_init(&proc->mutex, NULL);
	if (retval)
	{
	    errno = retval;
	    perror("error: pthread_mutex_init");
	    exit(EXIT_FAILURE);
	}
    }

    return proc;
}


void qgis_process_delete(struct qgis_process_s *proc)
{
    if (proc)
    {
	// ignore return value, maybe this file is already closed. I don't care
	close(proc->process_socket_fd);
	pthread_mutex_destroy(&proc->mutex);
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
	    fprintf(stderr, "warning: trying to set %s from %s for process %d\n",get_state_str(PROC_IDLE),get_state_str(proc->state),proc->pid);
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
	    fprintf(stderr, "warning: trying to set %s from %s for process %d\n",get_state_str(PROC_BUSY),get_state_str(proc->state),proc->pid);
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
	    fprintf(stderr, "warning: trying to set %s from %s for process %d\n",get_state_str(PROC_INIT),get_state_str(proc->state),proc->pid);
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



