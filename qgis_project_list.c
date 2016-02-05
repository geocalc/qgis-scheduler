/*
 * qgis_project_list.c
 *
 *  Created on: 10.01.2016
 *      Author: jh
 */



#include "qgis_project_list.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <sys/queue.h>
#include <errno.h>
#include <string.h>

#include "logger.h"


struct qgis_project_iterator
{
    LIST_ENTRY(qgis_project_iterator) entries;          /* Linked list prev./next entry */
    struct qgis_project_s *proj;
};

struct qgis_project_list_s
{
    LIST_HEAD(listhead, qgis_project_iterator) head;	/* Linked list head */
    pthread_rwlock_t rwlock;	/* lock used to protect list structures (add, remove, find, ..) */
};




struct qgis_project_list_s *qgis_proj_list_new(void)
{
    struct qgis_project_list_s *list = calloc(1, sizeof(*list));
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


void qgis_proj_list_delete(struct qgis_project_list_s *list)
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

	while (list->head.lh_first != NULL)
	{
	    struct qgis_project_iterator *entry = list->head.lh_first;
	    LIST_REMOVE(list->head.lh_first, entries);
	    qgis_project_delete(entry->proj);
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


void qgis_proj_list_add_project(struct qgis_project_list_s *list, struct qgis_project_s *proj)
{
    assert(list);
    if (list)
    {
	assert(proj);
	if (proj)
	{
	    struct qgis_project_iterator *entry = malloc(sizeof(*entry));
	    assert(entry);
	    if ( !entry )
	    {
		logerror("could not allocate memory");
		exit(EXIT_FAILURE);
	    }

	    entry->proj = proj;

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


void qgis_proj_list_remove_project(struct qgis_project_list_s *list, struct qgis_project_s *proj)
{
    assert(list);
    if (list)
    {
	assert(proj);
	if (proj)
	{
	    struct qgis_project_iterator *np;

	    int retval = pthread_rwlock_wrlock(&list->rwlock);
	    if (retval)
	    {
		errno = retval;
		logerror("error acquire read-write lock");
		exit(EXIT_FAILURE);
	    }

	    for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	    {
		if (proj == np->proj)
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


struct qgis_project_s *find_project_by_name(struct qgis_project_list_s *list, const char *name)
{
    struct qgis_project_s *proj = NULL;

    assert(list);
    if (list)
    {
	struct qgis_project_iterator *np;

	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	{
	    const char *projname = qgis_project_get_name(np->proj);
	    retval = strcmp(name, projname);
	    if ( !retval )
	    {
		proj = np->proj;
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

    return proj;
}


struct qgis_project_s *qgis_proj_list_find_project_by_pid(struct qgis_project_list_s *list, pid_t pid)
{
    struct qgis_project_s *proj = NULL;

    assert(list);
    if (list)
    {
	struct qgis_project_iterator *np;

	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	{
	    struct qgis_project_s *myproj = np->proj;
	    struct qgis_process_list_s *proc_list = qgis_project_get_process_list(myproj);
	    if (proc_list)
	    {
		// iterate through the list to find the relevant process
		struct qgis_process_s *proc = qgis_process_list_find_process_by_pid(proc_list, pid);
		if (proc)
		{
		    /* proc is not NULL, we found the process item.
		     * return the project which owns this process
		     */
		    proj = myproj;
		    break;
		}
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

    return proj;
}


struct qgis_project_iterator *qgis_proj_list_get_iterator(struct qgis_project_list_s *list)
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

	return list->head.lh_first;
    }

    return NULL;
}


struct qgis_project_s *qgis_proj_list_get_next_project(struct qgis_project_iterator **iterator)
{
    assert(iterator);
    if (iterator)
    {
	if (*iterator)
	{
	    struct qgis_project_s *proj = (*iterator)->proj;
	    *iterator = (*iterator)->entries.le_next;
	    return proj;
	}
    }

    return NULL;
}


void qgis_proj_list_return_iterator(struct qgis_project_list_s *list)
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


/* signals a dying process. The signal has been received by the signal trap.
 * now every project has to look, if the process is in its own process list.
 */
void qgis_proj_list_process_died(struct qgis_project_list_s *list, pid_t pid)
{
    assert(pid>0);
    assert(list);
    if (list)
    {
	struct qgis_project_iterator *np;

	int retval = pthread_rwlock_rdlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire read-write lock");
	    exit(EXIT_FAILURE);
	}

	for (np = list->head.lh_first; np != NULL; np = np->entries.le_next)
	{
	    struct qgis_project_s *myproj = np->proj;

	    /* every project has to look in its process lists if the pid
	     * matches an entry.
	     * remove the entry and in case restart the process
	     */
	    retval = qgis_project_process_died(myproj, pid);
	    if (retval)
		// found process, no need to look further
		break;
	}
	if (!retval)
	    // did not find any matching process
	    printlog("Process %d died, but did not find any matching project?", pid);

	retval = pthread_rwlock_unlock(&list->rwlock);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock read-write lock");
	    exit(EXIT_FAILURE);
	}
    }

}


