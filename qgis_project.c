/*
 * qgis_project.c
 *
 *  Created on: 10.01.2016
 *      Author: jh
 */

/*
    Database for the QGIS projects.
    Stores the project name and the list of processes working on that project.
    Also stores the path of the qgis configuration file for the processes and
    its inotify watch descriptor.

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


#include "qgis_project.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fastcgi.h>
#include <sys/inotify.h>
#include <libgen.h>	// used for dirname(), we need glibc >= 2.2.1 !!
#include <limits.h>

#include "qgis_process_list.h"
#include "qgis_config.h"
#include "qgis_inotify.h"
#include "fcgi_state.h"
#include "logger.h"
#include "timer.h"
#include "qgis_shutdown_queue.h"
#include "statistic.h"
#include "process_manager.h"


//#define DISABLED_INIT
#define MIN_PROCESS_RUNTIME_SEC		5
#define MIN_PROCESS_RUNTIME_NANOSEC	0

#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })


struct qgis_project_s
{
    struct qgis_process_list_s *initproclist;	// list of processes which are initialized and can not be used for requests
    struct qgis_process_list_s *activeproclist;	// list of processes which handle fcgi requests
    const char *name;
    const char *configpath;		// full path to qgis config file watched for updates. note: no watch if configpath is NULL
    const char *configbasename;		// only config file name without path
    pthread_t config_watch_threadid;
    pthread_rwlock_t rwlock;
    int inotifywatchfd;
    int nr_crashes;		// number of program crashes (i.e. process ends within warning time)
};


struct thread_watch_config_args
{
    struct qgis_project_s *project;
};






/* restarts all processes.
 * I.e. evaluate current number of processes for this project,
 * start num processes, init them,
 * atomically move the old processes to shutdown list
 * and new processes to active list,
 * and kill all old processes from shutdown list.
 */
static void qgis_project_restart_processes(struct qgis_project_s *project)
{
    assert(project);
    if (project)
    {
	const char *proj_name = project->name;
	int minproc = config_get_min_idle_processes(proj_name);
	int activeproc = qgis_process_list_get_num_process(project->activeproclist);
	int numproc = max(minproc, activeproc);
	process_manager_start_new_process_detached(numproc, project, 1);
    }
}


/* checks if the file name and watch descriptor belong to this project
 * initiate a process restart if the config did change.
 */
int qgis_project_check_inotify_config_changed(struct qgis_project_s *project, int wd)
{
    int ret = 0;

    if (wd == project->inotifywatchfd)
    {
	ret = 1;

	/* match, start new processes and then move them to idle list */
	printlog("Project '%s' config change. Restart processes", project->name);
	qgis_project_restart_processes(project);
    }

    return ret;
}


struct qgis_project_s *qgis_project_new(const char *name, const char *configpath)
{
    assert(name);
    //assert(configpath); configpath is allowed to be NULL. in this case no watcher thread is started

    struct qgis_project_s *proj = calloc(1, sizeof(*proj));
    assert(proj);
    if ( !proj )
    {
	logerror("could not allocate memory");
	exit(EXIT_FAILURE);
    }
    proj->initproclist = qgis_process_list_new();
    proj->activeproclist = qgis_process_list_new();
    proj->name = name;

    int retval = pthread_rwlock_init(&proj->rwlock, NULL);
    if (retval)
    {
	errno = retval;
	logerror("error init read-write lock");
	exit(EXIT_FAILURE);
    }

    /* if the path to a configuration file has been given and the path
     * is correct (stat()), then watch the file with inotify for changes
     * If the file changed, kill all processes and restart them anew.
     */
    if (configpath)
    {
	struct stat statbuf;
	retval = stat(configpath, &statbuf);
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
		logerror("error accessing file '%s': ", configpath);
		debug(1, "file is not watched for changes");
		break;

	    default:
		logerror("error accessing file '%s': ", configpath);
		exit(EXIT_FAILURE);
	    }
	}
	else
	{
	    if (S_ISREG(statbuf.st_mode))
	    {
		/* if I can stat the file I assume we can read it.
		 * Now setup the inotify descriptor.
		 */
		retval = qgis_inotify_watch_file(configpath);
		proj->inotifywatchfd = retval;

		proj->configbasename = basename((char *)configpath);
		proj->configpath = configpath;

	    }
	    else
	    {
		debug(1, "error '%s' is no regular file", configpath);
	    }
	}
    }

    return proj;
}


void qgis_project_delete(struct qgis_project_s *proj)
{
    if (proj)
    {
	debug(1, "deleting project '%s'", proj->name);

	qgis_process_list_delete(proj->initproclist);
	qgis_process_list_delete(proj->activeproclist);
	free(proj);
    }
}


int qgis_project_add_process(struct qgis_project_s *proj, struct qgis_process_s *proc)
{
    assert(proj);
    assert(proc);
    assert(proj->initproclist);
    if (proj && proj->initproclist && proc)
    {
	qgis_process_list_add_process(proj->initproclist, proc);

	return 0;
    }

    return -1;
}




/* move all processes from the lists to the shutdown module */
void qgis_project_shutdown(struct qgis_project_s *proj)
{
    assert(proj);

    if (proj)
    {
	qgis_shutdown_add_process_list(proj->initproclist);
	qgis_shutdown_add_process_list(proj->activeproclist);
    }
}


struct qgis_process_list_s *qgis_project_get_init_process_list(struct qgis_project_s *proj)
{
    struct qgis_process_list_s *list = NULL;

    assert(proj);
    if (proj)
    {
	list = proj->initproclist;
    }

    return list;
}


struct qgis_process_list_s *qgis_project_get_active_process_list(struct qgis_project_s *proj)
{
    struct qgis_process_list_s *list = NULL;

    assert(proj);
    if (proj)
    {
	list = proj->activeproclist;
    }

    return list;
}


const char *qgis_project_get_name(struct qgis_project_s *proj)
{
    const char *name = NULL;

    assert(proj);
    if (proj)
    {
	name = proj->name;
    }

    return name;
}


void qgis_project_print(struct qgis_project_s *proj)
{
    assert(proj);
    if (proj)
    {
	debug(1, "project %s, init process list", proj->name);
	qgis_process_list_print(proj->initproclist);
	debug(1, "project %s, active process list", proj->name);
	qgis_process_list_print(proj->activeproclist);
    }
}


void qgis_project_inc_nr_crashes(struct qgis_project_s *proj)
{
    assert(proj);
    if (proj)
    {
	proj->nr_crashes++;
    }
}


int qgis_project_get_nr_crashes(struct qgis_project_s *proj)
{
    int ret = -1;

    assert(proj);
    if (proj)
    {
	ret = proj->nr_crashes;
    }

    return ret;
}


void qgis_project_reset_nr_crashes(struct qgis_project_s *proj)
{
    assert(proj);
    if (proj)
    {
	proj->nr_crashes = 0;
    }
}


int qgis_project_get_inotify_fd(struct qgis_project_s *proj)
{
    int ret = -1;

    assert(proj);
    if (proj)
    {
	ret = proj->inotifywatchfd;
    }

    return ret;
}


