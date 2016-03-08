/*
 * process_manager.c
 *
 *  Created on: 04.03.2016
 *      Author: jh
 */

/*
    Management module for the processes.
    Acts on events to maintenance the processes.

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



#include "process_manager.h"

#include "database.h"
#include "qgis_config.h"
#include "qgis_shutdown_queue.h"
#include "project_manager.h"


static const int max_nr_process_crashes = 5;


/* a child process died.
 * this may happen because we cancelled its operation or
 * the process died because of a bug or low memory or something else.
 * Get the project this process was tasked for. If there is no project then the
 * process was already scheduled to shut down or the process id did not belong
 * to us. Either way in this case we don't need to take action.
 * Else look for the number of idle processes and restart a new process if
 * needed.
 */
void process_manager_process_died(pid_t pid)
{
    int retval = db_has_process(pid);
    if (retval)
    {
	/* check if we are during shutdown sequence. if not then restart the
	 * process.
	 * check if the process died during the initialisation sequence
	 * if it did, remark the process being instable.
	 * then start a new one.
	 * Refrain from restarting if too much processes have died during the
	 * initialization.
	 */
	retval = get_program_shutdown();
	if (!retval)
	{
	    const char *projname = db_get_project_for_this_process(pid);
	    enum db_process_list_e proclist = db_get_process_list(pid);
	    if (LIST_INIT == proclist)
	    {
		/* died during initialization. if this happens every time,
		 * we do get a startup loop. stop after some (5) tries.
		 */
		db_inc_startup_failures(projname);
	    }
	    retval = db_get_startup_failures(projname);
	    if (max_nr_process_crashes > retval)
	    {
		project_manager_start_new_process_detached(1, projname, 0);
	    }
	}

	/* change state of the process to STATE_EXIT
	 * and move the entry to the shutdown list
	 */
	db_process_set_state_exit(pid);
	qgis_shutdown_add_process(pid);
    }
    else
    {
	printlog("got signal SIGCHLD but pid %d does not belong to us", pid);
    }
//    db_remove_process(pid);
}


