/*
 * process_manager.h
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


#ifndef PROCESS_MANAGER_H_
#define PROCESS_MANAGER_H_

#include <sys/types.h>


void process_manager_process_died(pid_t pid);
void process_manager_process_died_during_init(pid_t pid, const char *projname);
void process_manager_start_new_process_wait(int num, const char *projname, int do_exchange_processes);
void process_manager_start_new_process_detached(int num, const char *projname, int do_exchange_processes);
void process_manager_cleanup_process(pid_t pid);


#endif /* PROCESS_MANAGER_H_ */
