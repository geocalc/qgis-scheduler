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

#include "qgis_process_list.h"
#include "qgis_config.h"


struct qgis_project_s
{
    struct qgis_process_list_s *proclist;	// list of processes which handle fcgi requests
    const char *name;
    const char *configpath;		// path to qgis config file watched for updates. note: no watch if configpath is NULL
    pthread_t config_watch_threadid;
    int does_shutdown;
    struct qgis_process_list_s *shutdownproclist; // list of processes which are shut down, no need to restart them
    pthread_rwlock_t rwlock;
};



/* does restart all processes belonging to this project.
 * We move the list of processes from proclist to shutdownproclist.
 * Then we start the required number of processes and initialize them.
 * The required number may be the number of processes from the old list
 * or the configuration parameter for minimal free idle processes.
 */
//static void qgis_project_restart_processes(struct qgis_project_s *proj)
//{
//    assert(proj);
//
//    int retval = pthread_rwlock_wrlock(&proj->rwlock);
//    if (retval)
//    {
//	errno = retval;
//	perror("error acquire read-write lock");
//	exit(EXIT_FAILURE);
//    }
//
//    if ( !proj->shutdownproclist )
//	proj->shutdownproclist = qgis_process_list_new();
//
//    qgis_process_list_transfer_all_process(proj->shutdownproclist, proj->proclist);
//
//    retval = pthread_rwlock_unlock(&proj->rwlock);
//    if (retval)
//    {
//	errno = retval;
//	perror("error unlock read-write lock");
//	exit(EXIT_FAILURE);
//    }
//
//}





/* watches changes in the configuration file.
 * uses inotify.
 */
//static void *thread_watch_config(void *arg)
//{
//
//    return NULL;
//}






struct qgis_project_s *qgis_project_new(const char *name, const char *configpath)
{
    assert(name);
    //assert(configpath); configpath is allowed to be NULL. in this case no watcher thread is started

    struct qgis_project_s *proj = calloc(1, sizeof(*proj));
    assert(proj);
    if ( !proj )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }
    proj->proclist = qgis_process_list_new();
    proj->name = name;
    proj->configpath = configpath;

    return proj;
}


void qgis_project_delete(struct qgis_project_s *proj)
{
    if (proj)
    {
	qgis_process_list_delete(proj->proclist);
	free(proj);
    }
}


//int qgis_project_add_process_list(struct qgis_project_s *proj, struct qgis_process_list_s *proclist)
//{
//    assert(proj);
//    assert(proclist);
//    if (proj && proclist)
//    {
//	proj->proclist = proclist;
//
//	return 0;
//    }
//
//    return -1;
//}


int qgis_project_add_process(struct qgis_project_s *proj, struct qgis_process_s *proc)
{
    assert(proj);
    assert(proc);
    assert(proj->proclist);
    if (proj && proj->proclist && proc)
    {
	qgis_process_list_add_process(proj->proclist, proc);

	return 0;
    }

    return -1;
}

/* bad, Bad, BAD workaround.
 * next time this function moves in here, and we get rid of this declaration
 */
void start_new_process_detached(int num, struct qgis_project_s *project);

/* some process died and sent its pid via SIGCHLD.
 * maybe it was listet in this project?
 * test and remove the old entry and maybe restart anew.
 */
void qgis_project_process_died(struct qgis_project_s *proj, pid_t pid)
{
    assert(proj);
    assert(pid>0);

    if (proj)
    {
	/* Erase the old entry. The process does not exist anymore */
	struct qgis_process_list_s *proclist = proj->proclist;
	assert(proclist);
	struct qgis_process_s *proc = qgis_process_list_find_process_by_pid(proclist, pid);
	if (proc)
	{
	    /* that process belongs to our active list.
	     * restart the process if not during shutdown.
	     */
	    qgis_process_list_remove_process(proclist, proc);
	    qgis_process_delete(proc);

	    if ( !get_program_shutdown() )
	    {
		fprintf(stderr, "restarting process\n");

		/* child process terminated, restart anew */
		/* TODO: react on child processes exiting immediately.
		 * maybe store the creation time and calculate the execution time?
		 */
		start_new_process_detached(1, proj);
	    }
	}
	else
	{
	    proclist = proj->shutdownproclist;
	    if (proclist)
	    {
		proc = qgis_process_list_find_process_by_pid(proclist, pid);
		if (proc)
		{
		    /* that process belongs to our list of deleted processes.
		     * just remove it from this list.
		     */
		    qgis_process_list_remove_process(proclist, proc);
		    qgis_process_delete(proc);
		}
	    }
	}
    }
}


int qgis_project_shutdown_process(struct qgis_project_s *proj, struct qgis_process_s *proc)
{

    return -1;
}


int qgis_project_shutdown_all_process(struct qgis_project_s *proj)
{

    return -1;
}


struct qgis_process_list_s *qgis_project_get_process_list(struct qgis_project_s *proj)
{
    struct qgis_process_list_s *list = NULL;

    assert(proj);
    if (proj)
    {
	list = proj->proclist;
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


