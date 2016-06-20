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
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>

#include "common.h"
#include "logger.h"
#include "timer.h"
#include "database.h"
#include "process_manager.h"
#include "qgis_config.h"




static pthread_t shutdownthread = 0;
static pthread_cond_t shutdowncondition = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t shutdownmutex = PTHREAD_MUTEX_INITIALIZER;
static int do_shutdown_thread = 0;
static int has_list_change = 0;
static int shutdown_main_pipe_wr = -1;


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
	struct timespec default_signal_timeout;
	int retval;

	/* get a list of all processes in the shutdown list.
	 * for each process evaluate:
	 * - is process timeout and state kill? set state exit
	 * - is process timeout and state term? send kill signal, set state
	 *   kill, start timer again. if process does not exist set state exit
	 * - is process timeout and state idle? send term signal, set state
	 *   term, start timer. if process does not exist set state exit
	 * - for any other process of the state term or kill do record the
	 *   timeout and evaluate next (minimal) timeout.
	 *
	 * Now erase all process with state exit from list.
	 *
	 * Are we in shutdown mode and is no entry left? then leave
	 *
	 * Is a timeout value given? then wait with timeout. else wait infinite
	 */

	struct timespec current_time;
	retval = qgis_timer_start(&current_time);
	if (retval)
	{
	    logerror("ERROR: retrieving time");
	    exit(EXIT_FAILURE);
	}

	pid_t *pidlist;
	int len;
	retval = db_get_list_process_by_list(&pidlist, &len, LIST_SHUTDOWN);
	// no need to check, retval is always 0

	retval = config_get_term_timeout();
	default_signal_timeout.tv_sec = retval / 1000;
	default_signal_timeout.tv_nsec = (((__typeof__ (default_signal_timeout.tv_nsec))retval)%1000)*1000*1000;

	struct timespec min_timer = {0};
	struct timespec proc_timer = {0};
	int i;
	for (i=0; i<len; i++)
	{
	    pid_t pid = pidlist[i];
	    enum db_process_state_e state = db_get_process_state(pid);
	    debug(1, "check pid %d, state %d", pid, state);
	    switch(state)
	    {
	    case PROC_STATE_START:
	    case PROC_STATE_INIT: // TODO: maybe wait until state changes to IDLE?
	    case PROC_STATE_IDLE:
	    case PROC_STATE_BUSY: // TODO: maybe wait until state changes to IDLE?
		/* we have an idle process. immediately send a term signal */
		retval = kill(pid, SIGTERM);
		debug(1, "kill(%d, SIGTERM) returned %d, errno %d", pid, retval, errno);
		if (-1 == retval)
		{
		    if (ESRCH == errno)
		    {
			process_manager_cleanup_process(pid);
		    }
		    else
		    {
			logerror("ERROR: calling kill(%d, SIGTERM)", pid);
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
			printlog("ERROR: can not set state to pid %d, unknown", pid);
			exit(EXIT_FAILURE);
		    }

		    retval = db_reset_signal_timer(pid);
		    if (-1 == retval)
		    {
			logerror("ERROR: setting the time value");
			exit(EXIT_FAILURE);
		    }
		}
		break;

	    case PROC_STATE_TERM:
		/* we have a process which has received a TERM signal.
		 * check the remaining signal time if it exceeds the timeout value.
		 * if it does send a kill signal
		 */
		retval = db_get_signal_timer(&proc_timer, pid);
		qgis_timer_add(&proc_timer, &default_signal_timeout);
		if ( qgis_timer_isgreaterthan(&current_time, &proc_timer) )
		{
		    printlog("timeout (%dsec) for process %d, sending SIGKILL signal", (int)default_signal_timeout.tv_sec, pid);
		    retval = kill(pid, SIGKILL);
		    if (-1 == retval)
		    {
			if (ESRCH == errno)
			{
			    process_manager_cleanup_process(pid);
			}
			else
			{
			    logerror("ERROR: calling kill(%d, SIGTERM)", pid);
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
			    printlog("ERROR: can not set state to pid %d, unknown", pid);
			    exit(EXIT_FAILURE);
			}

			retval = db_reset_signal_timer(pid);
			if (-1 == retval)
			{
			    logerror("ERROR: setting the time value");
			    exit(EXIT_FAILURE);
			}
		    }
		}
		break;

	    case PROC_STATE_KILL:
		/* still not gone?
		 * remove from db
		 */
		retval = db_get_signal_timer(&proc_timer, pid);
		qgis_timer_add(&proc_timer, &default_signal_timeout);
		if ( qgis_timer_isgreaterthan(&current_time, &proc_timer) )
		{
		    printlog("INFO: timeout (%dsec) for process %d. Could not kill process, please look after it", (int)default_signal_timeout.tv_sec, pid);
		    process_manager_cleanup_process(pid);
		}
		break;

	    case PROC_STATE_EXIT:
		/* zombi entry.
		 * just remove it from the lists.
		 */
		break;

	    case PROCESS_STATE_MAX:
		printlog("INFO: can not find process %d during shutdown, db changed inbetween data selects. ignoring process", pid);
		break;

	    default:
		printlog("ERROR: unexpected state value (%d) in shutdown list", state);
		exit(EXIT_FAILURE);
	    }
	}
	db_free_list_process(pidlist, len);
	pidlist = NULL;
	len = 0;

	/* we checked all processes in the shutdown list.
	 * now wheed out the processes with state exit
	 */
	db_remove_process_with_state_exit();

	/* get the minimal proc timer (or {0,0}) to see if we
	 * need to wait with timeout value.
	 */
	db_shutdown_get_min_signaltimer(&min_timer);

	/* wait for signal or new process or thread cancel request */
	retval = pthread_mutex_lock(&shutdownmutex);
	if (retval)
	{
	    errno = retval;
	    logerror("ERROR: can not lock mutex");
	    exit(EXIT_FAILURE);
	}

	/* if did did not get a call to qgis_shutdown_add_process() or
	 * qgis_shutdown_add_process_list() during our work in the lists
	 * then wait for a signal from other threads.
	 * else continue to work.
	 */
	if ( !has_list_change )
	{
	    if ( qgis_timer_is_empty(&min_timer) )
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
		    retval = qgis_timer_start(&min_timer);
		    if (retval)
		    {
			logerror("ERROR: can not get clock value");
			exit(EXIT_FAILURE);
		    }
		    static const struct timespec ts_timeout = {
			    tv_sec: 0,
			    tv_nsec: 200*1000*1000
		    };
		    qgis_timer_add(&min_timer, &ts_timeout);

		    struct timespec temp_ts;
		    qgis_timer_start(&temp_ts);
		    debug(1, "do shutdown and not signal timer set? current time: %ld,%03lds. wait %ld,%03ld seconds (until %ld,%03ld) or until next condition", temp_ts.tv_sec, (temp_ts.tv_nsec/(1000*1000)), ts_timeout.tv_sec, (ts_timeout.tv_nsec/(1000*1000)), min_timer.tv_sec, (min_timer.tv_nsec/(1000*1000)));
		    retval = pthread_cond_timedwait(&shutdowncondition, &shutdownmutex, &min_timer);
		}
		else
		{
		    debug(1, "wait until next condition");
		    retval = pthread_cond_wait(&shutdowncondition, &shutdownmutex);
		}
	    }
	    else
	    {
		qgis_timer_add(&min_timer, &default_signal_timeout);
		struct timespec temp_ts;
		qgis_timer_start(&temp_ts);
		debug(1, "current time: %ld,%03lds. wait until %ld,%03ld or until next condition", temp_ts.tv_sec, (temp_ts.tv_nsec/(1000*1000)), min_timer.tv_sec, (min_timer.tv_nsec/(1000*1000)));
		retval = pthread_cond_timedwait(&shutdowncondition, &shutdownmutex, &min_timer);
		debug(1, "pthread_cond_timedwait() returned %d", retval);
	    }

	    if ( 0 != retval && ETIMEDOUT != retval )
	    {
		errno = retval;
		logerror("ERROR: can not wait on condition");
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
	    logerror("ERROR: can not unlock mutex");
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

    /* write to main thread, we are done */
    struct signal_data_s sigdata;
    sigdata.signal = 0;
    sigdata.pid = 0;
    sigdata.is_shutdown = 1;

    int retval = write(shutdown_main_pipe_wr, &sigdata, sizeof(sigdata));
    if (-1 == retval)
    {
	logerror("ERROR: write signal data");
	exit(EXIT_FAILURE);
    }
    debug(1, "wrote %d bytes to sig pipe", retval);

    return NULL;
}


void qgis_shutdown_init(int main_pipe_wr)
{
    assert(0 <= main_pipe_wr);
    shutdown_main_pipe_wr = main_pipe_wr;

    /* initialize the condition variable timeout clock attribute with the same
     * value we use in the clock measurements. Else if we don't we have
     * different timeout values (clock module <-> pthread condition timeout)
     */
    pthread_condattr_t	condattr;
    int retval = pthread_condattr_init(&condattr);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: pthread_condattr_init");
	exit(EXIT_FAILURE);
    }
    retval = pthread_condattr_setclock(&condattr, get_valid_clock_id());
    if (retval)
    {
	errno = retval;
	logerror("ERROR: pthread_condattr_setclock() id %d", get_valid_clock_id());
	exit(EXIT_FAILURE);
    }
    retval = pthread_cond_init(&shutdowncondition, &condattr);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: pthread_cond_init");
	exit(EXIT_FAILURE);
    }
    retval = pthread_condattr_destroy(&condattr);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: pthread_condattr_destroy");
	exit(EXIT_FAILURE);
    }
    retval = pthread_create(&shutdownthread, NULL, qgis_shutdown_thread, NULL);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: creating thread");
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
	logerror("ERROR: joining thread");
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
	logerror("ERROR: can not lock mutex");
	exit(EXIT_FAILURE);
    }
    has_list_change = 1;
    retval = pthread_cond_signal(&shutdowncondition);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: can not wait on condition");
	exit(EXIT_FAILURE);
    }
    retval = pthread_mutex_unlock(&shutdownmutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: can not unlock mutex");
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
    debug(1, "notify shutdown list about change");

    int retval = pthread_mutex_lock(&shutdownmutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: can not lock mutex");
	exit(EXIT_FAILURE);
    }
    has_list_change = 1;
    retval = pthread_cond_signal(&shutdowncondition);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: can not wait on condition");
	exit(EXIT_FAILURE);
    }
    retval = pthread_mutex_unlock(&shutdownmutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: can not unlock mutex");
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
	logerror("ERROR: can not lock mutex");
	exit(EXIT_FAILURE);
    }
    do_shutdown_thread = 1;
    retval = pthread_cond_signal(&shutdowncondition);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: can not wait on condition");
	exit(EXIT_FAILURE);
    }
    retval = pthread_mutex_unlock(&shutdownmutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: can not unlock mutex");
	exit(EXIT_FAILURE);
    }
}


/* Called if a child process signalled its exit.
 * Then we can look in all lists to remove the process entry.
 */
//void qgis_shutdown_process_died(pid_t pid)
//{
//    debug(1,"process %d died", pid);
//
//    /* remove the process with "pid" from the lists,
//     * then signal the thread.
//     */
//    int retval = process_manager_cleanup_process(pid);
//    if (-1 == retval)
//    {
//	printlog("ERROR: signalled process %d not found in internal lists", pid);
//    }
//
//    retval = pthread_mutex_lock(&shutdownmutex);
//    if (retval)
//    {
//	errno = retval;
//	logerror("ERROR: can not lock mutex");
//	exit(EXIT_FAILURE);
//    }
//    has_list_change = 1;
//    retval = pthread_cond_signal(&shutdowncondition);
//    if (retval)
//    {
//	errno = retval;
//	logerror("ERROR: can not wait on condition");
//	exit(EXIT_FAILURE);
//    }
//    retval = pthread_mutex_unlock(&shutdownmutex);
//    if (retval)
//    {
//	errno = retval;
//	logerror("ERROR: can not unlock mutex");
//	exit(EXIT_FAILURE);
//    }
//}


