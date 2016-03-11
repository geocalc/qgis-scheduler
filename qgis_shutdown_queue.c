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
#include <pthread.h>

#include "logger.h"
#include "timer.h"
#include "database.h"
#include "qgis_config.h"


#define UNUSED_PARAMETER(x)	((void)(x))


static pthread_t shutdownthread = 0;
static pthread_cond_t shutdowncondition = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t shutdownmutex = PTHREAD_MUTEX_INITIALIZER;
static int do_shutdown_thread = 0;
static int has_list_change = 0;



static void *qgis_shutdown_thread(void *arg)
{
    UNUSED_PARAMETER(arg);

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

	pid_t pid = db_get_shutdown_process_in_timeout();
	while (-1 != pid)
	{
	    enum db_process_state_e state = db_get_process_state(pid);
	    switch(state)
	    {
	    case PROC_STATE_IDLE:
		/* send term signal */
		retval = kill(pid, SIGTERM);
		if (-1 == retval)
		{
		    if (ESRCH == errno)
		    {
			db_process_set_state_exit(pid);
		    }
		    else
		    {
			logerror("error calling kill(%d, SIGTERM)", pid);
			exit(EXIT_FAILURE);
		    }
		}
		else
		{
		    /* signal has been send. reset signal time to current time
		     * and change process state
		     */
		    retval = db_process_set_state(pid, PROC_STATE_TERM);
		    if (-1 == retval)
		    {
			printlog("error: can not set state to pid %d, unknown", pid);
			exit(EXIT_FAILURE);
		    }

		    retval = db_reset_signal_timer(pid);
		    if (-1 == retval)
		    {
			logerror("error setting the time value");
			exit(EXIT_FAILURE);
		    }
		}
		break;

	    case PROC_STATE_TERM:
		/* send kill signal */
		retval = kill(pid, SIGKILL);
		if (-1 == retval)
		{
		    if (ESRCH == errno)
		    {
			db_process_set_state_exit(pid);
		    }
		    else
		    {
			logerror("error calling kill(%d, SIGTERM)", pid);
			exit(EXIT_FAILURE);
		    }
		}
		else {
		    /* signal has been send. reset signal time to current time
		     * and change process state
		     */
		    retval = db_process_set_state(pid, PROC_STATE_KILL);
		    if (-1 == retval)
		    {
			printlog("error: can not set state to pid %d, unknown", pid);
			exit(EXIT_FAILURE);
		    }

		    retval = db_reset_signal_timer(pid);
		    if (-1 == retval)
		    {
			logerror("error setting the time value");
			exit(EXIT_FAILURE);
		    }
		}
		break;

	    case PROC_STATE_KILL:
		/* still not gone?
		 * remove from db
		 */
		db_process_set_state_exit(pid);
		break;

	    default:
		printlog("error: unexpected state value (%d) in shutdown list", state);
		exit(EXIT_FAILURE);
	    }

	    /* warning: better design necessary. this could lead to infinite
	     * loop if the current list item (i.e. process item) does not
	     * change. TODO
	     */
	    pid = db_get_shutdown_process_in_timeout();
	}

	/* clean up the resources for the process entries being in state EXIT
	 * i.e. they have no corresponding process anymore and shall be removed
	 */
	retval = db_remove_process_with_state_exit();
	debug(1, "removed %d processes with state exit", retval);

	/* get the timeout of the next signalling round.
	 * if no process is in the list "ts_sig" may be {0,0}
	 */
	struct timespec ts_sig = {0,0};
	db_shutdown_get_min_signaltimer(&ts_sig);
	debug(1, "min signal timer is %ld,%03lds", ts_sig.tv_sec, (ts_sig.tv_nsec/(1000*1000)));
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

		    debug(1, "wait %ld,%03ld seconds (until %ld,%03ld) or until next condition", ts_timeout.tv_sec, (ts_timeout.tv_nsec/(1000*1000)), ts_sig.tv_sec, (ts_sig.tv_nsec/(1000*1000)));
		    retval = pthread_cond_timedwait(&shutdowncondition, &shutdownmutex, &ts_sig);
		}
		else
		{
		    debug(1, "wait until next condition");
		    retval = pthread_cond_wait(&shutdowncondition, &shutdownmutex);
		}
	    }
	    else
	    {
		debug(1, "wait until %ld,%03ld or until next condition", ts_sig.tv_sec, (ts_sig.tv_nsec/(1000*1000)));
		retval = pthread_cond_timedwait(&shutdowncondition, &shutdownmutex, &ts_sig);
	    }

	    if ( 0 != retval && ETIMEDOUT != retval )
	    {
		errno = retval;
		logerror("error: can not wait on condition");
		exit(EXIT_FAILURE);
	    }
	}
	else
	{
	    debug(1, "list changed, reevaluate");
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
	    int num_list = db_get_num_shutdown_processes();
	    if ( 0 >= num_list )
	    {
		break;
	    }
	}

    }

    return NULL;
}


void qgis_shutdown_init()
{
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
    assert(shutdownthread);
    int retval = pthread_join(shutdownthread, NULL);
    if (retval)
    {
	errno = retval;
	logerror("error joining thread");
	exit(EXIT_FAILURE);
    }
    shutdownthread = 0;
}


/* Adds a process entry to the list of processes to end.
 * Do this before we call qgis_shutdown_wait_empty().
 */
void qgis_shutdown_add_process(pid_t pid)
{
    /* during shutdown sequence there may arrive a signal SIGCHLD from the
     * signal handler after we successfully did shut down this module.
     * Be a bit more relaxed during shutdown sequence.
     */
    int retval = get_program_shutdown();
    if (!retval)
	assert(!do_shutdown_thread);

    db_move_process_to_list(LIST_SHUTDOWN, pid);

    debug(1, "add one process to shutdown list");

    retval = pthread_mutex_lock(&shutdownmutex);
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


/* move all processes in init list and active list for this project to the
 * shutdown list.
 */
void qgis_shutdown_add_all_process(const char *project_name)
{
    db_move_all_process_from_init_to_shutdown_list(project_name);
    db_move_all_process_from_active_to_shutdown_list(project_name);
}


void qgis_shutdown_notify_changes(void)
{
    assert(!do_shutdown_thread);

    debug(1, "notify shutdown list about change");

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
//    assert(busylist);
    assert(!do_shutdown_thread);

    int retval = db_move_list_to_shutdown( list );
    debug(1, "moved %d processes to shutdown list", retval);

    qgis_shutdown_notify_changes();
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
}


/* Called if a child process signalled its exit.
 * Then we can look in all lists to remove the process entry.
 */
void qgis_shutdown_process_died(pid_t pid)
{
    debug(1,"process %d died", pid);

    /* remove the process with "pid" from the lists,
     * then signal the thread.
     */
    int retval = db_process_set_state_exit(pid);
    if (-1 == retval)
    {
	printlog("error: signalled process %d not found in internal lists", pid);
    }

    retval = pthread_mutex_lock(&shutdownmutex);
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


