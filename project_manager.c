/*
 * project_manager.c
 *
 *  Created on: 04.03.2016
 *      Author: jh
 */

/*
    Management module for the projects.
    Organize startup and shutdown of a whole project.

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


#include "project_manager.h"

#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include "common.h"
#include "database.h"
#include "qgis_config.h"
#include "logger.h"
#include "process_manager.h"
#include "qgis_inotify.h"



struct thread_start_project_processes_args
{
    char *project_name;
    int num;
};


static void *project_manager_thread_start_project_processes(void *arg)
{
    assert(arg);
    struct thread_start_project_processes_args *targ = arg;
    const char *projname = targ->project_name;
    int num = targ->num;

    assert(projname);
    assert(num >= 0);

    /* start "num" processes for this project and wait for them to finish
     * its initialization.
     * Then add this project to the global list
     */
    process_manager_start_new_process_wait(num, projname, 0);


    free(targ->project_name);
    free(arg);
    return NULL;
}


void project_manager_startup_projects(void)
{
    int retval;
    /* do for every project:
     * (TODO) check every project for correct configured settings.
     * Start a thread for every project, which in turn starts multiple
     *  child processes in parallel.
     * Wait for the project threads to finish.
     * After that we accept network connections.
     */

    int num_proj = config_get_num_projects();
    {
	pthread_t threads[num_proj];
	int i;
	for (i=0; i<num_proj; i++)
	{
	    int inotifyfd = 0;
	    const char *projname = config_get_name_project(i);
	    debug(1, "found project '%s'. Startup child processes", projname);

	    const char *configpath = config_get_project_config_path(projname);
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
			logerror("ERROR: accessing file '%s': ", configpath);
			debug(1, "file is not watched for changes");
			break;

		    default:
			logerror("ERROR: accessing file '%s': ", configpath);
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
			inotifyfd = retval;
		    }
		    else
		    {
			debug(1, "ERROR: '%s' is no regular file", configpath);
		    }
		}
	    }

	    db_add_project(projname, configpath, inotifyfd);

	    int nr_of_childs_during_startup	= config_get_min_idle_processes(projname);


	    struct thread_start_project_processes_args *targs = malloc(sizeof(*targs));
	    assert(targs);
	    if ( !targs )
	    {
		logerror("could not allocate memory");
		exit(EXIT_FAILURE);
	    }
	    targs->project_name = strdup(projname);
	    targs->num = nr_of_childs_during_startup;

	    retval = pthread_create(&threads[i], NULL, project_manager_thread_start_project_processes, targs);
	    if (retval)
	    {
		errno = retval;
		logerror("ERROR: creating thread");
		exit(EXIT_FAILURE);
	    }
	}

	for (i=0; i<num_proj; i++)
	{
	    retval = pthread_join(threads[i], NULL);
	    if (retval)
	    {
		errno = retval;
		logerror("ERROR: joining thread");
		exit(EXIT_FAILURE);
	    }
	}
    }
}


/* restarts all processes.
 * I.e. evaluate current number of processes for this project,
 * start num processes, init them,
 * atomically move the old processes to shutdown list
 * and new processes to active list,
 * and kill all old processes from shutdown list.
 */
static void project_manager_restart_processes(const char *proj_name)
{
    assert(proj_name);
    if (proj_name)
    {
	int minproc = config_get_min_idle_processes(proj_name);
	int activeproc = db_get_num_active_process(proj_name);
	int numproc = max(minproc, activeproc);
	process_manager_start_new_process_detached(numproc, proj_name, 1);
    }
}


void project_manager_projectname_configfile_changed(const char *projname)
{
    printlog("Project '%s' config change. Restart processes", projname);
    project_manager_restart_processes(projname);
}


/* A configuration with this watch descriptor has changed. Get the project
 * belonging to this descriptor and restart the project.
 */
void project_manager_project_configfile_changed(int inotifyid)
{
    assert(inotifyid >= 0);

    char *projname = db_get_project_for_inotifyid(inotifyid);
    if (projname)
    {
	project_manager_projectname_configfile_changed(projname);
    }
    else
    {
	printlog("ERROR: got config change request for inotify id %d but no project responsible?", inotifyid);
	exit(EXIT_FAILURE);
    }
    free(projname);
}


void project_manager_shutdown_project(const char *project_name)
{
//    debug(1, "shutdown project '%s'", project_name);
    printlog("shutdown project '%s'", project_name);
    int inotifyid = db_get_inotifyid_for_project(project_name);
    if (0 <= inotifyid)
    {
	qgis_inotify_delete_watch(inotifyid);
    }
    else
    {
	printlog("WARNING: got shutdown request for '%s' but no inotify id stored?", project_name);
    }
    db_move_all_process_from_init_to_shutdown_list(project_name);
    db_move_all_process_from_active_to_shutdown_list(project_name);
    db_remove_project(project_name);
}


void project_manager_shutdown(void)
{
    debug(1, "");

    char **projects;
    int len;
    db_get_names_project(&projects, &len);

    int i;
    for (i=0; i<len; i++)
    {
	project_manager_shutdown_project(projects[i]);
    }

    db_free_names_project(projects, len);

//    db_move_all_process_to_list(LIST_SHUTDOWN);
}


void project_manager_start_project(const char *projname)
{
    int inotifyfd = 0;
    int retval;
    const char *configpath = config_get_project_config_path(projname);
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
		logerror("ERROR: accessing file '%s': ", configpath);
		debug(1, "file is not watched for changes");
		break;

	    default:
		logerror("ERROR: accessing file '%s': ", configpath);
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
		inotifyfd = retval;
	    }
	    else
	    {
		debug(1, "ERROR: '%s' is no regular file", configpath);
	    }
	}
    }

    db_add_project(projname, configpath, inotifyfd);

    int nr_of_childs_during_startup	= config_get_min_idle_processes(projname);
    printlog("startup project '%s', starting %d processes", projname, nr_of_childs_during_startup);


    struct thread_start_project_processes_args *targs = malloc(sizeof(*targs));
    assert(targs);
    if ( !targs )
    {
	logerror("could not allocate memory");
	exit(EXIT_FAILURE);
    }
    targs->project_name = strdup(projname);
    targs->num = nr_of_childs_during_startup;

    pthread_attr_t attr;
    retval = pthread_attr_init(&attr);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: init thread attributes");
	exit(EXIT_FAILURE);
    }
    /* detach connection thread from the main thread. Doing this to collect
     * resources after this thread ends. Because there is no join() waiting
     * for this thread.
     */
    retval = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: setting attribute thread detached");
	exit(EXIT_FAILURE);
    }

    pthread_t thread;
    retval = pthread_create(&thread, &attr, project_manager_thread_start_project_processes, targs);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: creating thread");
	exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(&attr);

}


void project_manager_restart_project(const char *proj)
{
    project_manager_shutdown_project(proj);
    project_manager_start_project(proj);
}


/* receives a set of projects being new or deleted or having changed.
 * new projects are started according to their settings.
 * deleted projects are shut down, i.e. the processes are killed from memory.
 * changed projects are restarted, analog to being a deleted and new project in series.
 */
void project_manager_manage_project_changes(const char **newproj, const char **changedproj, const char **deletedproj)
{
    const char *proj;

    if (deletedproj)
	while ((proj = *deletedproj++))
	    project_manager_shutdown_project(proj);

    if (changedproj)
	while ((proj = *changedproj++))
	    project_manager_restart_project(proj);

    if (newproj)
	while ((proj = *newproj++))
	    project_manager_start_project(proj);
}


