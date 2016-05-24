/*
 * qgis_inotify.c
 *
 *  Created on: 05.02.2016
 *      Author: jh
 */

/*
    File change tracker.
    Provides a treat to sense changes in the configuration files which belong
    to the child processes.
    Calls a whole project list to check the changed inotify descriptor.

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


#include "qgis_inotify.h"

#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <sys/inotify.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <libgen.h>	// used for dirname(), we need glibc >= 2.2.1 !!

#include "common.h"
#include "database.h"
#include "qgis_config.h"
#include "logger.h"
#include "project_manager.h"

//#define OLD_STRUCT

struct inotify_watch
{
    char *filename;
    int watchfd;
};



static int inotifyfd = -1;
#ifdef OLD_STRUCT
static struct inotify_watch *watchlist = NULL;
static int watchlistlen = 0;
static int lastusedwatch = 0;
#endif
static pthread_rwlock_t inotifyrwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_t inotifythread = -1;


static void inotify_check_watchlist_for_watch(const struct inotify_event *inotifyevent)
{
    int i;
#ifdef OLD_STRUCT
    int len = lastusedwatch; // make local copy of global variable just in case the variable changes
    for (i=0; i<len; i++)
    {
	if (inotifyevent->wd == watchlist[i].watchfd)
	{
	    int retval = strcmp(inotifyevent->name, watchlist[i].filename);
	    if (0 == retval)
	    {
		project_manager_project_configfile_changed(i);
	    }
	}
    }
#else
    int len;
    char **list;
    db_get_projects_for_watchd_and_config(&list, &len, inotifyevent->wd, inotifyevent->name);
    for (i=0; i<len; i++)
    {
	project_manager_projectname_configfile_changed(list[i]);
    }
    db_delete_projects_for_watchd_and_config(list, len);
#endif
}



/* watches changes in the configuration file.
 * uses inotify.
 */
static void *inotify_thread_watch(void *arg)
{
    UNUSED_PARAMETER(arg);

    debug(1, "started inotify watcher thread");


    static const int sizeof_inotifyevent = sizeof(struct inotify_event) + NAME_MAX + 1;
    struct inotify_event *inotifyevent = malloc(sizeof_inotifyevent);
    assert(inotifyevent);
    if ( !inotifyevent )
    {
	logerror("ERROR: could not allocate memory");
	exit(EXIT_FAILURE);
    }

    assert(0 <= inotifyfd);
    for (;;)
    {
	int retval = read(inotifyfd, inotifyevent, sizeof_inotifyevent);
	if (-1 == retval)
	{
	    switch (errno)
	    {
	    case EINTR:
		/* We received an interrupt, possibly a termination signal.
		 */
		debug(1, "read() inotify_event received interrupt");
		break;

	    default:
		logerror("ERROR: read() inotify_event");
		exit(EXIT_FAILURE);
		// no break needed
	    }
	}
	else
	{
	    struct inotify_event *tmp_in_event = inotifyevent;
	    int size_read = retval;
	    debug(1, "inotify read %d bytes, sizeof event %lu + payload len %u", size_read, sizeof(*tmp_in_event), tmp_in_event->len);
	    int inotifyeventlen = sizeof(*tmp_in_event) + tmp_in_event->len;

	    while (size_read >= (int)(sizeof(*tmp_in_event) + tmp_in_event->len))
	    {
		switch(tmp_in_event->mask)
		{
		case IN_CLOSE_WRITE:
		    /* The file has been written or copied to this path
		     * Restart the processes
		     */
		    debug(1, "got event IN_CLOSE_WRITE");
		    debug(1, "mask 0x%x, len %d, name %s", tmp_in_event->mask, tmp_in_event->len, tmp_in_event->name);
		    inotify_check_watchlist_for_watch(tmp_in_event);
		    break;

		case IN_DELETE:
		    /* The file has been erased from this path.
		     * Don't care, just mark the service as not restartable. (or better close this project and kill child progs?)
		     */
		    debug(1, "got event IN_DELETE");
		    debug(1, "mask 0x%x, len %d, name %s", tmp_in_event->mask, tmp_in_event->len, tmp_in_event->name);
		    break;

		case IN_MOVED_TO:
		    /* The file has been overwritten by a move to this path
		     * Restart the processes
		     */
		    debug(1, "got event IN_MOVED_TO");
		    debug(1, "mask 0x%x, len %d, name %s", tmp_in_event->mask, tmp_in_event->len, tmp_in_event->name);
		    inotify_check_watchlist_for_watch(tmp_in_event);
		    break;

		case IN_IGNORED:
		    // Watch was removed. We can exit this thread
		    debug(1, "got event IN_IGNORED");
		    //		debug(1, "mask 0x%x, len %d, name %s", tmp_in_event->mask, tmp_in_event->len, tmp_in_event->name);
		    if (get_program_shutdown())
		    {
			goto thread_watch_config_end_for_loop;
		    }
		    break;

		default:
		    debug(1, "ERROR: got unexpected event %d", tmp_in_event->mask);
		    break;
		}

		if (size_read > (int)(sizeof(*tmp_in_event) + tmp_in_event->len))
		{
		    /* try to set the pointer to the next event location */
		    tmp_in_event = (struct inotify_event *)(((unsigned char *)tmp_in_event) + inotifyeventlen);
		    /* if the new length is greater than the size of the
		     * buffer then exit this loop.
		     */

		    size_read -= inotifyeventlen;
		    inotifyeventlen = sizeof(*tmp_in_event) + tmp_in_event->len;
		    assert(inotifyeventlen <= size_read);
		    if (inotifyeventlen > size_read)
		    {
			break;
		    }
		}
		else
		{
		    size_read -= inotifyeventlen;
		    inotifyeventlen = sizeof(*tmp_in_event) + tmp_in_event->len;
		}
	    }
	    assert(0 == size_read);

	}
    }

    thread_watch_config_end_for_loop:

    debug(1, "shutdown watcher thread");
    free(inotifyevent);

    return NULL;
}


void qgis_inotify_init(void)
{
#ifdef OLD_STRUCT
    /* NOTE: if we handle configuration reload without restarting this program
     * we need to care for this allocation as well!
     */
    watchlistlen = config_get_num_projects();
    watchlist = calloc(watchlistlen, sizeof(*watchlist));
    if (NULL == watchlist)
    {
	logerror("ERROR: calloc, can not get memory for inotify service");
	exit(EXIT_FAILURE);
    }
#endif

    int retval = inotify_init1(IN_CLOEXEC);
    if (-1 == retval)
    {
	logerror("ERROR: inotify_init1");
	exit(EXIT_FAILURE);
    }
    inotifyfd = retval;

    retval = pthread_create(&inotifythread, NULL, inotify_thread_watch, NULL);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: creating thread");
	exit(EXIT_FAILURE);
    }

}


void qgis_inotify_delete(void)
{
    int retval;
#ifdef OLD_STRUCT
    int i;
    for (i=0; i<watchlistlen; i++)
    {
	// TODO watchfd is used more than once
	retval = inotify_rm_watch(inotifyfd, watchlist[i].watchfd);
	if (-1 == retval)
	{
	    logerror("ERROR: can not remove inotify watch for watchfd %d", watchlist[i].watchfd);
	    // intentional no exit() call
	}
	free(watchlist[i].filename);
    }
#endif

    retval = pthread_join(inotifythread, NULL);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: joining thread");
	exit(EXIT_FAILURE);
    }

    retval = close(inotifyfd);
    debug(1, "closed inotify fd %d, retval %d, errno %d", inotifyfd, retval, errno);
    if (-1 == retval)
    {
	logerror("ERROR: can not close inotify fd");
	// intentional no exit() call
    }

#ifdef OLD_STRUCT
    free(watchlist);
#endif
}


int qgis_inotify_watch_file(const char *path)
{
    int ret = -1;

    assert (path);
    if (path)
    {
	struct stat statbuf;
	int retval = stat(path, &statbuf);
	if (-1 == retval)
	{
	    switch(errno)
	    {
	    case EACCES:
	    case ELOOP:
	    case EFAULT:
	    case ENAMETOOLONG:
	    case ENOENT:
	    case ENOTDIR:
	    case EOVERFLOW:
		logerror("ERROR: accessing file '%s': ", path);
		debug(1, "file is not watched for changes");
		break;

	    default:
		logerror("ERROR: accessing file '%s': ", path);
		exit(EXIT_FAILURE);
	    }
	}
	else
	{
	    if (S_ISREG(statbuf.st_mode))
	    {
		/* if I can stat the file I assume we can read it.
		 * Now setup the inotify descriptor.
		 *
		 * The process should be restarted if a new configuration is
		 * available.
		 * This may happen by editing the file in place (1), moving another
		 * file to the same place (2), copying another file to the same
		 * place (3) or creating the file in the directory (4).
		 * Looking at the man page we get:
		 * In case of (1) we receive the event IN_CLOSE_WRITE for the file
		 * and the directory as well.
		 * In case of (2) we receive the events IN_ATTRIB, IN_DELETE_SELF
		 * and IN_IGNORED for the configuration file, and IN_DELETE and
		 * IN_MOVED_TO for the directory.
		 * In case of (3) we receive the event IN_CLOSE_WRITE for the
		 * directory.
		 * In case of (4) we receive the event IN_CLOSE_WRITE for the
		 * directory.
		 * If the file is deleted the project may reject starting further
		 * processes and keep the current ones to feed the network clients.
		 *
		 * With some tests we get:
		 * Looking at the directory and the target file name alone we get
		 * the event IN_CLOSE_WRITE for the case (1), (3) and (4) and the
		 * event IN_MOVED_TO for the case (2).
		 *
		 * So in summary we watch the directory for changes and react on
		 * the file name of the configuration file.
		 *
		 * Currently we do not handle deleting and creating of the whole
		 * directory. This case may expose some bugs though..
		 */

#ifdef OLD_STRUCT
		assert(lastusedwatch < watchlistlen);
#endif

		char *directoryname = strdup(path);
		assert(directoryname);
		if ( !directoryname )
		{
		    logerror("ERROR: could not allocate memory");
		    exit(EXIT_FAILURE);
		}

		directoryname = dirname(directoryname);

		retval = pthread_rwlock_wrlock(&inotifyrwlock);
		if (retval)
		{
		    errno = retval;
		    logerror("ERROR: acquire read-write lock");
		    exit(EXIT_FAILURE);
		}

#ifdef OLD_STRUCT
		const char *configbasename = basename((char *)path);
		watchlist[lastusedwatch].filename = strdup(configbasename);
		if ( !watchlist[lastusedwatch].filename )
		{
		    logerror("ERROR: could not allocate memory");
		    exit(EXIT_FAILURE);
		}
#endif
		/* NOTE: if we call inotify_add_watch() multiple times with the
		 *       same 'directoryname' then it returns the same value.
		 */
		retval = inotify_add_watch(inotifyfd, directoryname, IN_CLOSE_WRITE|IN_DELETE|IN_MOVED_TO|IN_IGNORED);
		if (-1 == retval)
		{
		    logerror("ERROR: inotify_add_watch");
		    exit(EXIT_FAILURE);
		}
#ifdef OLD_STRUCT
		watchlist[lastusedwatch].watchfd = retval;
		ret = lastusedwatch;

		lastusedwatch++;
#endif

		ret = db_add_new_inotifyid(path, retval);

		retval = pthread_rwlock_unlock(&inotifyrwlock);
		if (retval)
		{
		    errno = retval;
		    logerror("ERROR: unlock read-write lock");
		    exit(EXIT_FAILURE);
		}


		free(directoryname);
	    }
	    else
	    {
		//debug(1, "ERROR: '%s' is no regular file", configpath);
		printlog("INFO: Inotify can not watch '%s', no regular file", path);
	    }
	}
    }

    return ret;
}


/* deletes the inotify watch for the file which is identified by "watchid".
 */
void qgis_inotify_delete_watch(int inotifyid)
{
    /* check the number of inotifyids which also got this watch descriptor.
     * if the number is <= 1 we can remove the watch from this file.
     */
    int watchnum = db_get_num_of_similar_watches_for_inotifyid(inotifyid);
    debug(1, "number of projects %d are watching with same watchd as inotify id %d", watchnum, inotifyid);
    if (watchnum <= 1)
    {
	int watchd = db_get_watchd_for_inotifyid(inotifyid);
	debug(1, "remove inotify watchd %d", watchd);
	int retval = inotify_rm_watch(inotifyfd, watchd);
	if (-1 == retval)
	{
	    logerror("ERROR: can not remove inotify watch for watch descriptor %d", watchd);
	    exit(EXIT_FAILURE);
	}
    }
    db_remove_inotifyid(inotifyid);
}



