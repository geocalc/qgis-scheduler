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

#include "database.h"
#include "qgis_config.h"
#include "logger.h"
#include "qgis_project_list.h"


struct thread_start_project_processes_args
{
    struct qgis_project_s *project;
    int num;
};


void *thread_start_project_processes(void *arg)
{
    assert(arg);
    struct thread_start_project_processes_args *targ = arg;
    struct qgis_project_s *project = targ->project;
    int num = targ->num;

    assert(project);
    assert(num >= 0);

    /* start "num" processes for this project and wait for them to finish
     * its initialization.
     * Then add this project to the global list
     */
    qgis_proj_list_add_project(db_get_active_project_list(), project);
    qgis_project_start_new_process_wait(num, project, 0);


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
		const char *projname = config_get_name_project(i);
		debug(1, "found project '%s'. Startup child processes", projname);

		const char *configpath = config_get_project_config_path(projname);
		struct qgis_project_s *project = qgis_project_new(projname, configpath);

		int nr_of_childs_during_startup	= config_get_min_idle_processes(projname);


		struct thread_start_project_processes_args *targs = malloc(sizeof(*targs));
		assert(targs);
		if ( !targs )
		{
		    logerror("could not allocate memory");
		    exit(EXIT_FAILURE);
		}
		targs->project = project;
		targs->num = nr_of_childs_during_startup;

		retval = pthread_create(&threads[i], NULL, thread_start_project_processes, targs);
		if (retval)
		{
		    errno = retval;
		    logerror("error creating thread");
		    exit(EXIT_FAILURE);
		}
	    }

	    for (i=0; i<num_proj; i++)
	    {
		retval = pthread_join(threads[i], NULL);
		if (retval)
		{
		    errno = retval;
		    logerror("error joining thread");
		    exit(EXIT_FAILURE);
		}
	    }
	}
}


void project_manager_start_new_process_detached(int num, const char *projectname, int do_exchange_processes)
{
    struct qgis_project_s *project = find_project_by_name(db_get_active_project_list(), projectname);

    qgis_project_start_new_process_detached(num, project, do_exchange_processes);

}
