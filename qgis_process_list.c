/*
 * qgis_process_list.c
 *
 *  Created on: 08.01.2016
 *      Author: jh
 */

/*
    List manager for the QGIS processes.
    Provides helper functions to search in the list.

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


#include "qgis_process_list.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <sys/queue.h>
#include <errno.h>
#include <signal.h>

#include "logger.h"
#include "timer.h"


struct qgis_process_list_s
{
    LIST_HEAD(listhead, qgis_process_iterator) head;	/* Linked list head */
    pthread_rwlock_t rwlock;	/* lock used to protect list structures (add, remove, find, ..) */
};

struct qgis_process_iterator
{
    LIST_ENTRY(qgis_process_iterator) entries;          /* Linked list prev./next entry */
    struct qgis_process_s *proc;
};


struct qgis_process_list_s *qgis_process_list_new(void)
{
    struct qgis_process_list_s *list = calloc(1, sizeof(*list));
    assert(list);
    if ( !list )
    {
	logerror("could not allocate memory");
	exit(EXIT_FAILURE);
    }
    LIST_INIT(&list->head);	// same as calloc(), should we remove this?
    int retval = pthread_rwlock_init(&list->rwlock, NULL);
    if (retval)
    {
	errno = retval;
	logerror("error init read-write lock");
	exit(EXIT_FAILURE);
    }

    return list;
}

void qgis_process_list_delete(struct qgis_process_list_s *list)
{
    if (list)
    {
	int retval = pthread_rwlock_wrlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	while ( !LIST_EMPTY(&list->head) )
	{
	    struct qgis_process_iterator *entry = LIST_FIRST(&list->head);
	    LIST_REMOVE(LIST_FIRST(&list->head), entries);
	    qgis_process_delete(entry->proc);
	    free(entry);
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
	retval = pthread_rwlock_destroy(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error delete read-write lock");
	    exit(EXIT_FAILURE);
	}
	free(list);
    }
}


void qgis_process_list_add_process(struct qgis_process_list_s *list, struct qgis_process_s *proc)
{
    assert(list);
    if (list)
    {
	assert(proc);
	if (proc)
	{
	    struct qgis_process_iterator *entry = malloc(sizeof(*entry));
	    assert(entry);
	    if ( !entry )
	    {
		logerror("could not allocate memory");
		exit(EXIT_FAILURE);
	    }

	    entry->proc = proc;

	    int retval = pthread_rwlock_wrlock(&list->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error acquire read-write lock");
		exit(EXIT_FAILURE);
	    }

	    LIST_INSERT_HEAD(&list->head, entry, entries);      /* Insert at the head. */

	    retval = pthread_rwlock_unlock(&list->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error unlock read-write lock");
		exit(EXIT_FAILURE);
	    }
	}
    }
}


void qgis_process_list_remove_process(struct qgis_process_list_s *list, struct qgis_process_s *proc)
{
    assert(list);
    if (list)
    {
	assert(proc);
	if (proc)
	{
	    struct qgis_process_iterator *np;
	    int retval = pthread_rwlock_wrlock(&list->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error acquire read-write lock");
		exit(EXIT_FAILURE);
	    }

	    LIST_FOREACH(np, &list->head, entries)
	    {
		if (proc == np->proc)
		{
		    LIST_REMOVE(np, entries);
		    free(np);
		    break;
		}
	    }

	    retval = pthread_rwlock_unlock(&list->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error unlock read-write lock");
		exit(EXIT_FAILURE);
	    }
	}
    }
}


/* removes all process entries with specifice "state" from the list and calls
 * delete() on this process entries
 */
int qgis_process_list_delete_all_process_with_state(struct qgis_process_list_s *list, enum qgis_process_state_e state)
{
    int ret = 0;

    assert(list);
    if (list)
    {
	    struct qgis_process_iterator *np;
	    int retval = pthread_rwlock_wrlock(&list->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error acquire read-write lock");
		exit(EXIT_FAILURE);
	    }

	    /* Note: we can not use LIST_FOREACH() because the list structure
	     * will be modified during the loop.
	     * TODO: get the source of LIST_FOREACH_SAFE() which can do this
	     */
	    np = LIST_FIRST(&list->head);
	    while ( np != NULL )
	    {
		struct qgis_process_iterator *next = LIST_NEXT(np, entries);
		struct qgis_process_s *proc = np->proc;
		assert(proc);

		enum qgis_process_state_e mystate = qgis_process_get_state(proc);
		if (mystate == state)
		{
		    qgis_process_delete(proc);
		    LIST_REMOVE(np, entries);
		    free(np);
		    ret++;
		}

		np = next;
	    }

	    retval = pthread_rwlock_unlock(&list->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error unlock read-write lock");
		exit(EXIT_FAILURE);
	    }
    }

    return ret;
}


/* moves all processes with a specific state from one list to another */
int qgis_process_list_transfer_all_process_with_state(struct qgis_process_list_s *tolist, struct qgis_process_list_s *fromlist, enum qgis_process_state_e state)
{
    int ret = 0;

    assert(tolist);
    assert(fromlist);
    if (fromlist && tolist)
    {
	    struct qgis_process_iterator *np;

	    int retval = pthread_rwlock_wrlock(&fromlist->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error acquire read-write lock");
		exit(EXIT_FAILURE);
	    }
	    retval = pthread_rwlock_wrlock(&tolist->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error acquire read-write lock");
		exit(EXIT_FAILURE);
	    }

	    /* Note: this could be faster if we use a tail queue (TAILQ) or a
	     * single pointer tail queue (STAILQ) and the *_CONCAT() macro
	     */
	    np = LIST_FIRST(&fromlist->head);
	    while ( np != NULL )
	    {
		struct qgis_process_iterator *next = LIST_NEXT(np, entries);

		pthread_mutex_t *mutex = qgis_process_get_mutex(np->proc);
		retval = pthread_mutex_lock(mutex);
		if (retval)
		{
		    errno = retval;
		    logerror("error acquire mutex");
		    exit(EXIT_FAILURE);
		}

		if (qgis_process_get_state(np->proc) == state)
		{
		    LIST_REMOVE(np, entries);
		    LIST_INSERT_HEAD(&tolist->head, np, entries);
		    ret++;
		}

		retval = pthread_mutex_unlock(mutex);
		if (retval)
		{
		    errno = retval;
		    logerror("error unlock mutex");
		    exit(EXIT_FAILURE);
		}

		np = next;
	    }

	    retval = pthread_rwlock_unlock(&tolist->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error unlock read-write lock");
		exit(EXIT_FAILURE);
	    }
	    retval = pthread_rwlock_unlock(&fromlist->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error unlock read-write lock");
		exit(EXIT_FAILURE);
	    }

    }

    return ret;
}


void qgis_process_list_transfer_process(struct qgis_process_list_s *tolist, struct qgis_process_list_s *fromlist, const struct qgis_process_s *proc)
{
    assert(tolist);
    assert(fromlist);
    if (fromlist && tolist)
    {
	struct qgis_process_iterator *np;

	int retval = pthread_rwlock_wrlock(&fromlist->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}
	retval = pthread_rwlock_wrlock(&tolist->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &fromlist->head, entries)
	{
	    const struct qgis_process_s *myproc = np->proc;
	    if (proc == myproc)
	    {
		LIST_REMOVE(np, entries);
		LIST_INSERT_HEAD(&tolist->head, np, entries);
		break;
	    }
	}

	retval = pthread_rwlock_unlock(&tolist->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
	retval = pthread_rwlock_unlock(&fromlist->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}

    }
}


int qgis_process_list_transfer_all_process(struct qgis_process_list_s *tolist, struct qgis_process_list_s *fromlist)
{
    int ret = 0;

    assert(tolist);
    assert(fromlist);
    if (fromlist && tolist)
    {
	    struct qgis_process_iterator *np;

	    int retval = pthread_rwlock_wrlock(&fromlist->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error acquire read-write lock");
		exit(EXIT_FAILURE);
	    }
	    retval = pthread_rwlock_wrlock(&tolist->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error acquire read-write lock");
		exit(EXIT_FAILURE);
	    }

	    /* note: this could be faster if we just concatenate the two lists
	     */
	    for (np = LIST_FIRST(&fromlist->head); np != NULL; np = LIST_FIRST(&fromlist->head))
	    {
		    LIST_REMOVE(np, entries);
		    LIST_INSERT_HEAD(&tolist->head, np, entries);
		    ret++;
	    }

	    retval = pthread_rwlock_unlock(&tolist->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error unlock read-write lock");
		exit(EXIT_FAILURE);
	    }
	    retval = pthread_rwlock_unlock(&fromlist->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error unlock read-write lock");
		exit(EXIT_FAILURE);
	    }

    }

    return ret;
}


//struct process_s *find_process_by_threadid(struct qgis_process_list_s *list, pthread_t id)
//{
//    assert(list);
//
//    struct entry *np;
//    for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
//        if (id == np->proc.threadid)
//            return &np->proc;
//
//    return NULL;
//}
//


/* Looks for process entries with their status IDLE and return them
 * (atomically) with their status changed to BUSY.
 * This way the connections threads can get an idle process without race
 * conditions.
 * The thread id of the calling thread is written to the process dataset.
 */
struct qgis_process_s *qgis_process_list_find_idle_return_busy(struct qgis_process_list_s *list)
{
    struct qgis_process_s *proc = NULL;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}


	LIST_FOREACH(np, &list->head, entries)
	{
	    pthread_mutex_t *mutex = qgis_process_get_mutex(np->proc);
	    retval = pthread_mutex_lock(mutex);
	    if (retval)
	    {
		errno = retval;
		logerror("error acquire mutex");
		exit(EXIT_FAILURE);
	    }

	    enum qgis_process_state_e mystate = qgis_process_get_state(np->proc);
	    if (PROC_IDLE == mystate)
	    {
		/* we found a process idling.
		 * change its status to busy and return the process entry
		 */
		/* NOTE: This may take some time:
		 * Get a lock on the mutex if another thread just holds the
		 * lock. If there are many starting network connections at the
		 * same time we may see threads waiting on one another. I.e.
		 * thread1 gets the lock, tries and gets a proc busy state.
		 * Meanwhile thread2 tries to get the lock and waits for
		 * thread1. Then thread1 releases the lock, gets the next lock
		 * and reads the process state information. Meanwhile thread2
		 * gets the first lock, again reads the state being busy and
		 * then tries to get the lock which is held by thread1.
		 * How about testing for a lock and if it is held by another
		 * thread go on to the next?
		 */
		retval = qgis_process_set_state_busy(np->proc, pthread_self());
		if ( !retval )
		{
		    // set state BUSY is ok
		    proc = np->proc;
		}
	    }

	    retval = pthread_mutex_unlock(mutex);
	    if (retval)
	    {
		errno = retval;
		logerror("error unlock mutex");
		exit(EXIT_FAILURE);
	    }

	    if (NULL != proc)
		// we found a matching process. no need to look further
		break;
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }

    return proc;
}


struct qgis_process_s *qgis_process_list_find_process_by_status(struct qgis_process_list_s *list, enum qgis_process_state_e state)
{
    struct qgis_process_s *proc = NULL;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &list->head, entries)
	{
	    enum qgis_process_state_e mystate = qgis_process_get_state(np->proc);
	    if (state == mystate)
	    {
		proc = np->proc;
		break;
	    }
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }

    return proc;
}


struct qgis_process_s *qgis_process_list_mutex_find_process_by_status(struct qgis_process_list_s *list, enum qgis_process_state_e state)
{
    struct qgis_process_s *proc = NULL;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &list->head, entries)
	{
	    pthread_mutex_t *mutex = qgis_process_get_mutex(np->proc);
	    retval = pthread_mutex_lock(mutex);
	    if (retval)
	    {
		errno = retval;
		logerror("error acquire mutex");
		exit(EXIT_FAILURE);
	    }

	    enum qgis_process_state_e mystate = qgis_process_get_state(np->proc);
	    if (state == mystate)
	    {
		/* we found a process idling.
		 * keep the lock on this process until the thread
		 * has put itself on the busy list
		 * of this process
		 */
		/* NOTE: This may take some time:
		 * Get a lock on the mutex if another thread just holds the
		 * lock. If there are many starting network connections at the
		 * same time we may see threads waiting on one another. I.e.
		 * thread1 gets the lock, tries and gets a proc busy state.
		 * Meanwhile thread2 tries to get the lock and waits for
		 * thread1. Then thread1 releases the lock, gets the next lock
		 * and reads the process state information. Meanwhile thread2
		 * gets the first lock, again reads the state being busy and
		 * tries to get the lock which is held by thread1.
		 * How about testing for a lock and if it is held by another
		 * thread go on to the next?
		 */
		proc = np->proc; // intentionally return entry with mutex locked
		break;
	    }

	    retval = pthread_mutex_unlock(mutex);
	    if (retval)
	    {
		errno = retval;
		logerror("error unlock mutex");
		exit(EXIT_FAILURE);
	    }
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }

    return proc;
}


struct qgis_process_s *qgis_process_list_find_process_by_pid(struct qgis_process_list_s *list, pid_t pid)
{
    struct qgis_process_s *proc = NULL;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &list->head, entries)
	{
	    pid_t mypid = qgis_process_get_pid(np->proc);
	    if (pid == mypid)
	    {
		proc = np->proc;
		break;
	    }
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }

    return proc;
}


struct qgis_process_s *qgis_process_list_get_first_process(struct qgis_process_list_s *list)
{
    struct qgis_process_s *proc = NULL;

    assert(list);
    if (list)
    {
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	if ( !LIST_EMPTY(&list->head) )
	{
	    proc = LIST_FIRST(&list->head)->proc;
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }

    return proc;
}


struct qgis_process_iterator *qgis_process_list_get_iterator(struct qgis_process_list_s *list)
{
    assert(list);
    if (list)
    {
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	return LIST_FIRST(&list->head);
    }

    return NULL;
}

struct qgis_process_s *qgis_process_list_get_next_process(struct qgis_process_iterator **iterator)
{
    assert(iterator);
    if (iterator)
    {
	if (*iterator)
	{
	    struct qgis_process_s *proc = (*iterator)->proc;
	    *iterator = LIST_NEXT(*iterator, entries);
	    return proc;
	}
    }

    return NULL;
}

void qgis_process_list_return_iterator(struct qgis_process_list_s *list)
{
    assert(list);
    if (list)
    {
	int retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }
}


int qgis_process_list_get_pid_list(struct qgis_process_list_s *list, pid_t **pid, int *len)
{
    assert(list);
    assert(pid);
    assert(len);
    if (list && pid && len)
    {
	struct qgis_process_iterator *np;
	int count = 0;

	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &list->head, entries)
	{
	    count++;
	}

	pid_t *pidp = calloc(count, sizeof(*pidp));
	assert(pidp);
	if ( !pidp )
	{
	    logerror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}

	int i = 0;
	LIST_FOREACH(np, &list->head, entries)
	{
	    pidp[i++] = qgis_process_get_pid(np->proc);
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}

	assert(count == i);

	*pid = pidp;
	*len = count;

	return 0;
    }

    return -1;
}

int qgis_process_list_get_num_process(struct qgis_process_list_s *list)
{
    int count = 0;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &list->head, entries)
	{
	    count++;
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }

    return count;
}


int qgis_process_list_get_num_process_by_status(struct qgis_process_list_s *list, enum qgis_process_state_e state)
{
    int count = 0;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &list->head, entries)
	{
	    enum qgis_process_state_e mystate = qgis_process_get_state(np->proc);
	    if (state == mystate)
	    {
		count++;
	    }
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }

    return count;
}


/* send a signal to all programs in the list
 * side effect: removes a process from the list, if its pid is not available (process not running).
 * return: number of processes send the signal */
int qgis_process_list_send_signal(struct qgis_process_list_s *list, int signal)
{
    int num = 0;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_wrlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	for (np = LIST_FIRST(&list->head); np != NULL; )
	{
	    /* get the next entry in advance. because maybe the list order
	     * is changed in the loop
	     */
	    struct qgis_process_iterator *next = LIST_NEXT(np, entries);

	    struct qgis_process_s *myproc = np->proc;
	    pid_t pid = qgis_process_get_pid(myproc);

	    retval = kill(pid, signal);
	    if (0 > retval)
	    {
		switch(errno)
		{
		case ESRCH:
		    /* child process is not existent anymore.
		     * erase it from the list of available processes
		     */
		{
		    LIST_REMOVE(np, entries);
		    qgis_process_delete(myproc);
		}
		break;
		default:
		    logerror("error: could not send TERM signal");
		}
	    }
	    else
	    {
		num++;
	    }

	    np = next;
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }

    return num;
}


/* sends an appropriate shutdown signal to every process in this list */
void qgis_process_list_signal_shutdown(struct qgis_process_list_s *list)
{
    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &list->head, entries)
	{
	    qgis_process_signal_shutdown(np->proc);
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }
}


/* Returns the earliest signal timer found in the list of processes.
 * Or 0,0 if no signal timer is found in the list or the list is empty.
 */
void qgis_process_list_get_min_signaltimer(struct qgis_process_list_s *list, struct timespec *maxtimeval)
{
    assert(list);
    if (list)
    {
	struct timespec maxts = {0,0};

	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &list->head, entries)
	{
	    const struct timespec *ts = qgis_process_get_signaltime(np->proc);

	    if ( !qgis_timer_is_empty(ts))
		if ( qgis_timer_isgreaterthan(&maxts, ts) || qgis_timer_is_empty(&maxts) )
		    maxts = *ts;
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}

	*maxtimeval = maxts;
    }
}


void qgis_process_list_print(struct qgis_process_list_s *list)
{
    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &list->head, entries)
	{
	    struct qgis_process_s *proc = np->proc;
	    qgis_process_print(proc);
	}

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }
}


/* returns the next process which had received a signal some "timeout" ago
 * or never received a signal
 */
struct qgis_process_s *get_next_shutdown_proc(struct qgis_process_list_s *list)
{
    struct qgis_process_s *ret = NULL;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	LIST_FOREACH(np, &list->head, entries)
	{
	    struct qgis_process_s *proc = np->proc;
	    enum qgis_process_state_e state = qgis_process_get_state(proc);
	    switch (state)
	    {
	    case PROC_IDLE:
	    //case PROC_EXIT:
	    {
		    ret = proc;
		    goto list_end;
	    }
	    case PROC_TERM:
	    case PROC_KILL:
	    {
		const struct timespec *tm = qgis_process_get_signaltime(proc);
		if (qgis_timer_is_empty(tm))
		{
		    /* this process never received a signal?? return this process */
		    ret = proc;
		    goto list_end;
		}
		struct timespec tmsub;
		retval = qgis_timer_sub(tm, &tmsub);
		if (-1 == retval)
		{
		    logerror("error getting time");
		    exit(EXIT_FAILURE);
		}
		    static const struct timespec ts_timeout = {
			    tv_sec: 10,
			    tv_nsec: 0
		    };
		retval = qgis_timer_isgreaterthan(&tmsub, &ts_timeout);
		if (retval)
		{
		    ret = proc;
		    goto list_end;
		}
		break;
	    }
	    default:
		break;
	    }
	}
	list_end:

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }

    return ret;
}


