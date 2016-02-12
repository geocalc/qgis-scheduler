/*
 * qgis_shutdown_queue.c
 *
 *  Created on: 10.02.2016
 *      Author: jh
 */

/*
    Separate queue to shutdown child processes.
    It gets the process descriptions and sends them a kill signal and
    waits until the process ended and no more threads are working with them.

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


#include "qgis_shutdown_queue.h"

#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>

#include "qgis_process_list.h"
#include "logger.h"
#include "timer.h"



static struct qgis_process_list_s *shutdownlist = NULL;	// list pf processes to be killed and removed
static struct qgis_process_list_s *busylist = NULL;	// list of processes being state busy or added via api
static pthread_t shutdownthread = 0;
static pthread_cond_t shutdowncondition = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t shutdownmutex = PTHREAD_MUTEX_INITIALIZER;
static int do_shutdown_thread = 0;
static int has_list_change = 0;



static void *qgis_shutdown_thread(void *arg)
{
    /* Algorithm:
     *
     * Normal operation:
     * Send a signal to all processes to terminate.
     * If the process has been signalled 10 seconds ago then send a SIGKILL
     * signal.
     * If the process has ended AND its status changed to PROC_IDLE we can
     * remove the process entry from the list.
     * Then wait for a signal SIGCHILD (qgis_shutdown_process_died()) or an
     * entry added to the list (qgis_shutdown_add_process()) or a shutdown
     * signal (qgis_shutdown_wait_empty()).
     *
     * Shutdown operation:
     * We assume that no more processes are added to the shutdown list.
     * proceed like the normal operation until all processes are removed from
     * the shutdown list. Then exit this thread.
     */

    for (;;)
    {
	int retval;

	/* look through all processes in the busy list if the status changed.
	 * if it does change to shutdown list */
	qgis_process_list_transfer_all_process_with_state(shutdownlist, busylist, PROC_IDLE);
	qgis_process_list_transfer_all_process_with_state(shutdownlist, busylist, PROC_OPEN_IDLE);

	/* send a signal to all processes in the shutdown list.
	 * If the process already died, set its state to EXIT.
	 */
	qgis_process_list_signal_shutdown(shutdownlist);

	/* clean the list from exited processes */
	if ( qgis_process_list_get_num_process_by_status(shutdownlist, PROC_EXIT) )
	{
	    /* TODO: Bad bad workaround. do this more efficiently in
	     * qgis_process_list
	     */
	    struct qgis_process_list_s *exitlist = qgis_process_list_new();
	    qgis_process_list_transfer_all_process_with_state(exitlist, shutdownlist, PROC_EXIT);
	    qgis_process_list_delete(exitlist);
	}


	/* get the timeout of the next signalling round.
	 * if no process is in the list "ts_sig" may be {0,0}
	 */
	struct timespec ts_sig = {0,0};
	qgis_process_list_get_min_signaltimer(shutdownlist, &ts_sig);
	if ( !qgis_timer_is_empty(&ts_sig) )
	{
	    qgis_timer_add(&ts_sig, &default_signal_timeout);
	}


	/* wait for signal or new process or thread cancel request */
	retval = pthread_mutex_lock(&shutdownmutex);
	if (retval)
	{
	    errno = retval;
	    logerror("error: can not lock mutex");
	    exit(EXIT_FAILURE);
	}

	/* if did did not get a call to qgis_shutdown_add_process() or
	 * qgis_shutdown_add_process_list() during our work in the lists
	 * then wait for a signal from other threads.
	 * else continue to work.
	 */
	if ( !has_list_change )
	{
	    if ( qgis_timer_is_empty(&ts_sig) )
	    {
		if (do_shutdown_thread)
		{
		    /* We have been signalled to shut down. And we have no
		     * processes which have been signalled to shut down? Should not
		     * happen but you'll never know. maybe there is a process
		     * waiting in the busylist to get idle.
		     *
		     * Set a small timeout of 0.2 seconds in case we do not exit
		     * this loop immediately.
		     */
		    retval = qgis_timer_start(&ts_sig);
		    if (retval)
		    {
			logerror("error: can not get clock value");
			exit(EXIT_FAILURE);
		    }
		    static const struct timespec ts_timeout = {
			    tv_sec: 0,
			    tv_nsec: 200*1000*1000
		    };
		    qgis_timer_add(&ts_sig, &ts_timeout);

		    retval = pthread_cond_timedwait(&shutdowncondition, &shutdownmutex, &ts_sig);
		}
		else
		    retval = pthread_cond_wait(&shutdowncondition, &shutdownmutex);
	    }
	    else
		retval = pthread_cond_timedwait(&shutdowncondition, &shutdownmutex, &ts_sig);

	    if ( 0 != retval && ETIMEDOUT != retval )
	    {
		errno = retval;
		logerror("error: can not wait on condition");
		exit(EXIT_FAILURE);
	    }
	}

	has_list_change = 0; // reset api caller semaphore


	/* note: "do_shutdown_thread" is under protection from "shutdownmutex".
	 * Make a local copy of the value so we can move the evaluation
	 * beond pthread_mutex_unlock().
	 */
	int local_do_shutdown_thread = do_shutdown_thread;

	retval = pthread_mutex_unlock(&shutdownmutex);
	if (retval)
	{
	    errno = retval;
	    logerror("error: can not unlock mutex");
	    exit(EXIT_FAILURE);
	}

	if (local_do_shutdown_thread)
	{
	    int num_list = qgis_process_list_get_num_process(shutdownlist);
	    if ( 0 >= num_list )
	    {
		num_list = qgis_process_list_get_num_process(busylist);
		if ( 0 >= num_list )
		{
		    break;
		}
	    }
	}

    }

    return NULL;
}


void qgis_shutdown_init()
{
    shutdownlist = qgis_process_list_new();
    busylist = qgis_process_list_new();

    int retval = pthread_create(&shutdownthread, NULL, qgis_shutdown_thread, NULL);
    if (retval)
    {
	errno = retval;
	logerror("error creating thread");
	exit(EXIT_FAILURE);
    }

}


void qgis_shutdown_delete()
{
    qgis_process_list_delete(shutdownlist);
    qgis_process_list_delete(busylist);
}


/* Adds a process entry to the list of processes to end.
 * Do this before we call qgis_shutdown_wait_empty().
 */
void qgis_shutdown_add_process(struct qgis_process_s *proc)
{
    assert(!do_shutdown_thread);
    assert(busylist);

    qgis_process_list_add_process( busylist, proc );

    int retval = pthread_mutex_lock(&shutdownmutex);
    if (retval)
    {
	errno = retval;
	logerror("error: can not lock mutex");
	exit(EXIT_FAILURE);
    }
    has_list_change = 1;
    retval = pthread_cond_signal(&shutdowncondition);
    if (retval)
    {
	errno = retval;
	logerror("error: can not wait on condition");
	exit(EXIT_FAILURE);
    }
    retval = pthread_mutex_unlock(&shutdownmutex);
    if (retval)
    {
	errno = retval;
	logerror("error: can not unlock mutex");
	exit(EXIT_FAILURE);
    }
}


/* moves an entire list of processes to the shutdown module */
void qgis_shutdown_add_process_list(struct qgis_process_list_s *list)
{
    assert(busylist);
    assert(!do_shutdown_thread);

    qgis_process_list_transfer_all_process( busylist, list );

    int retval = pthread_mutex_lock(&shutdownmutex);
    if (retval)
    {
	errno = retval;
	logerror("error: can not lock mutex");
	exit(EXIT_FAILURE);
    }
    has_list_change = 1;
    retval = pthread_cond_signal(&shutdowncondition);
    if (retval)
    {
	errno = retval;
	logerror("error: can not wait on condition");
	exit(EXIT_FAILURE);
    }
    retval = pthread_mutex_unlock(&shutdownmutex);
    if (retval)
    {
	errno = retval;
	logerror("error: can not unlock mutex");
	exit(EXIT_FAILURE);
    }
}


/* Used during the shutdown transition of the main thread.
 * It signals the thread to end its work if all processes have been removed
 * from the lists.
 */
void qgis_shutdown_wait_empty(void)
{
    /* send a signal to the thread to clean up all lists.
     * then wait for the thread to return.
     */
    int retval = pthread_mutex_lock(&shutdownmutex);
    if (retval)
    {
	errno = retval;
	logerror("error: can not lock mutex");
	exit(EXIT_FAILURE);
    }
    do_shutdown_thread = 1;
    retval = pthread_cond_signal(&shutdowncondition);
    if (retval)
    {
	errno = retval;
	logerror("error: can not wait on condition");
	exit(EXIT_FAILURE);
    }
    retval = pthread_mutex_unlock(&shutdownmutex);
    if (retval)
    {
	errno = retval;
	logerror("error: can not unlock mutex");
	exit(EXIT_FAILURE);
    }


    assert(shutdownthread);
    retval = pthread_join(shutdownthread, NULL);
    if (retval)
    {
	errno = retval;
	logerror("error joining thread");
	exit(EXIT_FAILURE);
    }
    shutdownthread = 0;
}


/* Called if a child process signalled its exit.
 * Then we can look in all lists to remove the process entry.
 */
void qgis_shutdown_process_died(pid_t pid)
{
    assert(0);
}


