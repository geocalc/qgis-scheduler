/*
 * database.h
 *
 *  Created on: 03.03.2016
 *      Author: jh
 */

/*
    Database for the project and process data.
    Provides information about all current projects, processes and statistics.

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


#ifndef DATABASE_H_
#define DATABASE_H_

#include <sys/types.h>


enum db_process_state_e
{
    PROC_STATE_START = 0,	// process has started and needs to be initialized
    PROC_STATE_INIT,		// process gets initialized
    PROC_STATE_IDLE,		// process is initialized and ready to work
    PROC_STATE_OPEN_IDLE,// deprecated! do not use. TODO: remove this item

    PROC_STATE_BUSY,		// process is busy with an fcgi request
    PROC_STATE_TERM,		// process received the term signal
    PROC_STATE_KILL,		// process received the kill signal
    PROC_STATE_EXIT,		// process is not existend anymore

    PROCESS_STATE_MAX	// last entry. do not use
};

/* The livecycle of a process runs through these three lists.
 * First the process gets initialized. In this list the process can not do
 * useful work, so the processes in that list are not taken to answer web
 * requests. The process may exit this list after the initialization phase with
 * the state idle or by crashing. All existing (i.e. idle) processes exit this
 * list to the active list.
 *
 * The second list (active list) is the work list. Processes in here answer
 * requests from the web server, or they idle around. The process in here may
 * exit this list with a request to shut down or by crashing. All existing
 * (i.e. not killed) processes exit this list to the shutdown list.
 *
 * The third list is the shutdown list. In this list the processes wont accept
 * further work from the webserver, but the may end their current work if it is
 * a long running task. As soon as the processes in this list get idle, they
 * receive a shutdown signal (SIGTERM or SIGKILL).
 *
 * This enumeration describes the lists.
 */
enum db_process_list_e
{
    LIST_INIT,
    LIST_ACTIVE,
    LIST_SHUTDOWN,

    LIST_SELECTOR_MAX	// last entry. do not use
};



void db_init(void);
void db_shutdown(void);
void db_delete(void);


void db_add_project(const char *projname);
void db_add_process(const char *projname, pid_t pid);
int db_get_num_idle_process(const char *projname);
const char *db_get_project_for_this_process(pid_t pid);
void db_remove_process(pid_t pid);
pid_t db_get_process(const char *projname, enum db_process_list_e list, enum db_process_state_e state);
pid_t db_get_next_idle_process_for_work(const char *projname);
int db_get_process_socket(pid_t pid);
int db_process_set_state_idle(pid_t pid);
int db_get_num_process_by_status(const char *projname, enum db_process_state_e state);

/* transitional interfaces. these are deleted after the api change */
struct qgis_project_list_s;

struct qgis_project_list_s *db_get_active_project_list(void);



#endif /* DATABASE_H_ */
