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
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }
    LIST_INIT(&list->head);	// same as calloc(), should we remove this?
    pthread_rwlock_init(&list->rwlock, NULL);

    return list;
}

void qgis_process_list_delete(struct qgis_process_list_s *list)
{
    if (list)
    {
	pthread_rwlock_wrlock(&list->rwlock);
	while (list->head.lh_first != NULL)
	{
	    struct qgis_process_iterator *entry = list->head.lh_first;
	    LIST_REMOVE(list->head.lh_first, entries);
	    qgis_process_delete(entry->proc);
	    free(entry);
	}
	pthread_rwlock_unlock(&list->rwlock);
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
		perror("could not allocate memory");
		exit(EXIT_FAILURE);
	    }
	    entry->proc = proc;
	    pthread_rwlock_wrlock(&list->rwlock);
	    LIST_INSERT_HEAD(&list->head, entry, entries);      /* Insert at the head. */
	    pthread_rwlock_unlock(&list->rwlock);
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
	    pthread_rwlock_wrlock(&list->rwlock);
	    for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	    {
		if (proc == np->proc)
		{
		    LIST_REMOVE(np, entries);
		    free(np);
		    break;
		}
	    }
	    pthread_rwlock_unlock(&list->rwlock);
	}
    }
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

struct qgis_process_s *qgis_process_list_find_process_by_status(struct qgis_process_list_s *list, enum qgis_process_state_e state)
{
    struct qgis_process_s *retval = NULL;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	pthread_rwlock_rdlock(&list->rwlock);
	for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	{
	    enum qgis_process_state_e mystate = qgis_process_get_state(np->proc);
	    if (state == mystate)
	    {
		retval = np->proc;
		break;
	    }
	}
	pthread_rwlock_unlock(&list->rwlock);
    }

    return retval;
}


struct qgis_process_s *qgis_process_list_mutex_find_process_by_status(struct qgis_process_list_s *list, enum qgis_process_state_e state)
{
    struct qgis_process_s *retval = NULL;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	pthread_rwlock_rdlock(&list->rwlock);
	for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	{
	    pthread_mutex_t *mutex = qgis_process_get_mutex(np->proc);
	    pthread_mutex_lock(mutex);
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
		retval = np->proc; // intentionally return entry with mutex locked
		break;
	    }
	    pthread_mutex_unlock(mutex);
	}
	pthread_rwlock_unlock(&list->rwlock);
    }

    return retval;
}


struct qgis_process_s *qgis_process_list_find_process_by_pid(struct qgis_process_list_s *list, pid_t pid)
{
    struct qgis_process_s *retval = NULL;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	pthread_rwlock_rdlock(&list->rwlock);
	for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	{
	    pid_t mypid = qgis_process_get_pid(np->proc);
	    if (pid == mypid)
	    {
		retval = np->proc;
		break;
	    }
	}
	pthread_rwlock_unlock(&list->rwlock);
    }

    return retval;
}


struct qgis_process_s *qgis_process_list_get_first_process(struct qgis_process_list_s *list)
{
    struct qgis_process_s *retval = NULL;

    assert(list);
    if (list)
    {
	pthread_rwlock_rdlock(&list->rwlock);
	if (list->head.lh_first)
	{
	    retval = list->head.lh_first->proc;
	}
	pthread_rwlock_unlock(&list->rwlock);
    }

    return retval;
}


struct qgis_process_iterator *qgis_process_list_get_iterator(struct qgis_process_list_s *list)
{
    assert(list);
    if (list)
    {
	pthread_rwlock_rdlock(&list->rwlock);
	return list->head.lh_first;
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
	    *iterator = (*iterator)->entries.le_next;
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
	pthread_rwlock_unlock(&list->rwlock);
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

	pthread_rwlock_rdlock(&list->rwlock);

	for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	{
	    count++;
	}

	pid_t *pidp = calloc(count, sizeof(*pidp));
	assert(pidp);
	if ( !pidp )
	{
	    perror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	int i = 0;
	for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	{
	    pidp[i++] = qgis_process_get_pid(np->proc);
	}

	pthread_rwlock_unlock(&list->rwlock);
	assert(count == i);

	*pid = pidp;
	*len = count;

	return 0;
    }

    return -1;
}


int qgis_process_list_get_num_process_by_status(struct qgis_process_list_s *list, enum qgis_process_state_e state)
{
    int count = 0;

    assert(list);
    if (list)
    {
	struct qgis_process_iterator *np;
	pthread_rwlock_rdlock(&list->rwlock);
	for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	{
	    enum qgis_process_state_e mystate = qgis_process_get_state(np->proc);
	    if (state == mystate)
	    {
		count++;
	    }
	}
	pthread_rwlock_unlock(&list->rwlock);
    }

    return count;
}




