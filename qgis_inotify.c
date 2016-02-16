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

#include "qgis_project_list.h"
#include "qgis_config.h"
#include "logger.h"


struct inotify_watch
{
    char *filename;
    int watchfd;
};


static int inotifyfd = -1;
static struct inotify_watch *watchlist = NULL;
static int watchlistlen = 0;
static int lastusedwatch = 0;
static pthread_rwlock_t inotifyrwlock = PTHREAD_RWLOCK_INITIALIZER;
static struct qgis_project_list_s *inotifyprojectlist = NULL;
static pthread_t inotifythread = -1;


static void inotify_check_watchlist_for_watch(struct inotify_event *inotifyevent)
{
    int i;
    int len = lastusedwatch; // make local copy of global variable just in case the variable changes
    for (i=0; i<len; i++)
    {
	if (inotifyevent->wd == watchlist[i].watchfd)
	{
	    int retval = strcmp(inotifyevent->name, watchlist[i].filename);
	    if (0 == retval)
	    {
		qgis_proj_list_config_change(inotifyprojectlist, i);
	    }
	}
    }
}



/* watches changes in the configuration file.
 * uses inotify.
 */
static void *inotify_thread_watch(void *arg)
{
//    assert(arg);
//    struct thread_watch_config_args *tinfo = arg;
//    struct qgis_project_s *project = tinfo->project;
//    assert(project);
    pthread_t thread_id = pthread_self();


//    const char *projname = project->name;
    debug(1, "[%lu] started inotify watcher thread\n", thread_id);


    static const int sizeof_inotifyevent = sizeof(struct inotify_event) + NAME_MAX + 1;
    struct inotify_event *inotifyevent = malloc(sizeof_inotifyevent);
    assert(inotifyevent);
    if ( !inotifyevent )
    {
	logerror("could not allocate memory");
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
		debug(1, "[%lu] read() inotify_event received interrupt\n", thread_id);
		break;

	    default:
		logerror("read() inotify_event");
		exit(EXIT_FAILURE);
		// no break needed
	    }
	}
	else
	{
	    struct inotify_event *tmp_in_event = inotifyevent;
	    int size_read = retval;
	    debug(1, "[%lu] inotify read %d bytes, sizeof event %lu, len %u\n", thread_id, size_read, sizeof(*tmp_in_event), tmp_in_event->len);
	    int inotifyeventlen = sizeof(*tmp_in_event) + tmp_in_event->len;

	    while (size_read >= sizeof(*tmp_in_event) + tmp_in_event->len)
	    {
		switch(tmp_in_event->mask)
		{
		case IN_CLOSE_WRITE:
		    /* The file has been written or copied to this path
		     * Restart the processes
		     */
		    debug(1, "got event IN_CLOSE_WRITE\n");
		    debug(1, "mask 0x%x, len %d, name %s\n", tmp_in_event->mask, tmp_in_event->len, tmp_in_event->name);
		    inotify_check_watchlist_for_watch(tmp_in_event);
		    break;

		case IN_DELETE:
		    /* The file has been erased from this path.
		     * Don't care, just mark the service as not restartable. (or better close this project and kill child progs?)
		     */
		    debug(1, "got event IN_DELETE\n");
		    debug(1, "mask 0x%x, len %d, name %s\n", tmp_in_event->mask, tmp_in_event->len, tmp_in_event->name);
		    break;

		case IN_MOVED_TO:
		    /* The file has been overwritten by a move to this path
		     * Restart the processes
		     */
		    debug(1, "got event IN_MOVED_TO\n");
		    debug(1, "mask 0x%x, len %d, name %s\n", tmp_in_event->mask, tmp_in_event->len, tmp_in_event->name);
		    inotify_check_watchlist_for_watch(tmp_in_event);
		    break;

		case IN_IGNORED:
		    // Watch was removed. We can exit this thread
		    debug(1, "got event IN_IGNORED\n");
		    //		debug(1, "mask 0x%x, len %d, name %s\n", tmp_in_event->mask, tmp_in_event->len, tmp_in_event->name);
		    if (get_program_shutdown())
		    {
			goto thread_watch_config_end_for_loop;
		    }
		    break;

		default:
		    debug(1, "error: got unexpected event %d\n", tmp_in_event->mask);
		    break;
		}

		if (size_read > sizeof(*tmp_in_event) + tmp_in_event->len)
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

    debug(1, "[%lu] shutdown watcher thread\n", thread_id);
    free(inotifyevent);
//    free(arg);
    return NULL;
}


void qgis_inotify_init(struct qgis_project_list_s *projectlist)
{
    assert(projectlist);
    /* NOTE: if we handle configuration reload without restarting this program
     * we need to care for this allocation as well!
     */
    watchlistlen = config_get_num_projects();
    watchlist = calloc(watchlistlen, sizeof(*watchlist));
    if (NULL == watchlist)
    {
	logerror("calloc, can not get memory for inotify service");
	exit(EXIT_FAILURE);
    }

    int retval = inotify_init1(IN_CLOEXEC);
    if (-1 == retval)
    {
	logerror("inotify_init1");
	exit(EXIT_FAILURE);
    }
    inotifyfd = retval;

    inotifyprojectlist = projectlist;

    retval = pthread_create(&inotifythread, NULL, inotify_thread_watch, NULL);
    if (retval)
    {
	errno = retval;
	logerror("error creating thread");
	exit(EXIT_FAILURE);
    }

}


void qgis_inotify_delete(void)
{
    int retval;
    int i;
    for (i=0; i<watchlistlen; i++)
    {
	// TODO watchfd is used more than once
	retval = inotify_rm_watch(inotifyfd, watchlist[i].watchfd);
	if (-1 == retval)
	{
	    logerror("can not remove inotify watch for watchfd %d", watchlist[i].watchfd);
	    // intentional no exit() call
	}
	free(watchlist[i].filename);
    }

    retval = pthread_join(inotifythread, NULL);
    if (retval)
    {
	errno = retval;
	logerror("error joining thread");
	exit(EXIT_FAILURE);
    }

    retval = close(inotifyfd);
    debug(1, "closed inotify fd %d, retval %d, errno %d", inotifyfd, retval, errno);
    if (-1 == retval)
    {
	logerror("can not close inotify fd");
	// intentional no exit() call
    }

    free(watchlist);
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
		logerror("error accessing file '%s': ", path);
		debug(1, "file is not watched for changes\n");
		break;

	    default:
		logerror("error accessing file '%s': ", path);
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

		assert(lastusedwatch < watchlistlen);

		char *directoryname = strdup(path);
		assert(directoryname);
		if ( !directoryname )
		{
		    logerror("could not allocate memory");
		    exit(EXIT_FAILURE);
		}

		directoryname = dirname(directoryname);

		retval = pthread_rwlock_wrlock(&inotifyrwlock);
		if (retval)
		{
		    errno = retval;
		    logerror("error acquire read-write lock");
		    exit(EXIT_FAILURE);
		}

		const char *configbasename = basename((char *)path);
		watchlist[lastusedwatch].filename = strdup(configbasename);
		if ( !watchlist[lastusedwatch].filename )
		{
		    logerror("could not allocate memory");
		    exit(EXIT_FAILURE);
		}

		retval = inotify_add_watch(inotifyfd, directoryname, IN_CLOSE_WRITE|IN_DELETE|IN_MOVED_TO|IN_IGNORED);
		if (-1 == retval)
		{
		    logerror("inotify_add_watch");
		    exit(EXIT_FAILURE);
		}
		watchlist[lastusedwatch].watchfd = retval;

		ret = lastusedwatch;

		lastusedwatch++;

		retval = pthread_rwlock_unlock(&inotifyrwlock);
		if (retval)
		{
		    errno = retval;
		    logerror("error unlock read-write lock");
		    exit(EXIT_FAILURE);
		}


		free(directoryname);
	    }
	    else
	    {
		//debug(1, "error '%s' is no regular file\n", configpath);
		printlog("INFO: Inotify can not watch '%s', no regular file", path);
	    }
	}
    }

    return ret;
}




