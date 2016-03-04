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
    db_remove_process(pid);
}
