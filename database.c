/*
 * database.c
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



#include "database.h"

#include <stdlib.h>
#include <sqlite3.h>
#include <pthread.h>
#include <assert.h>

#include "logger.h"


/* process data:
 * process id, list state, process state, worker thread id, socket fd
 *
 * project data:
 * project name, number of crashes during init phase
 */




static sqlite3 *dbhandler = NULL;

/* transitional data. this will be deleted after the code change */
#include "qgis_project_list.h"

struct qgis_project_list_s *projectlist = NULL;




enum qgis_process_state_e db_state_to_qgis_state(enum db_process_state_e state)
{
    return (enum qgis_process_state_e)state;
}

enum db_process_state_e qgis_state_to_db_state(enum qgis_process_state_e state)
{
    return (enum db_process_state_e)state;
}


/* called by sqlite3 function sqlite3_log()
 */
static void db_log(void *obsolete, int result, const char *msg)
{
    (void)obsolete;
    /* TODO doku says we have to be thread save.
     * is it sufficient to rely on printlog(...) ?
     */
    debug(1, "SQlite3: retval %d, %s", result, msg);
}


void db_init(void)
{
    int retval = sqlite3_config(SQLITE_CONFIG_SERIALIZED);
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite3_config(): %s\n"
		"Can not set thread safe access mode\n"
		"Did you compile sqlite3 with 'SQLITE_THREADSAFE=1'?", sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }

    retval = sqlite3_config(SQLITE_CONFIG_LOG, db_log, NULL);
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite3_config(): %s\n"
		"Can not set thread safe access mode\n"
		"Did you compile sqlite3 with 'SQLITE_THREADSAFE=1'?", sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }

    retval = sqlite3_initialize();
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite3_initialize(): %s", sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }

    retval = sqlite3_open(":memory:", &dbhandler);
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite3_open(): %s", sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }
    debug(1, "created memory db");

    /* setup all tables */
    static const char sql[] = "CREATE TABLE projects (name TEXT)";
    char *errormsg;
    retval = sqlite3_exec(dbhandler, sql, NULL, NULL, &errormsg);
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite with '%s': %s", sql, errormsg);
	exit(EXIT_FAILURE);
    }


    projectlist = qgis_proj_list_new();


}


void db_delete(void)
{
    /* remove the projects */
    qgis_proj_list_delete(projectlist);


    int retval = sqlite3_close(dbhandler);
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite3_close(): %s", sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }

    retval = sqlite3_shutdown();
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite3_shutdown(): %s", sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }
    debug(1, "shutdown memory db");
}


void db_add_project(const char *projname);
void db_add_process(const char *projname, pid_t pid);
int db_get_num_idle_process(const char *projname);


const char *db_get_project_for_this_process(pid_t pid)
{
    const char *ret = NULL;

    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);

    if (project)
	ret = qgis_project_get_name(project);

    return ret;
}


void db_remove_process(pid_t pid)
{
    /* child process died, rearrange the project list */
    qgis_proj_list_process_died(projectlist, pid);
}


/* find a process in a certain list with a distinct state
 * Note:
 * Using the pid as key index value may create problems, if a process dies
 * and another process gets the same pid.
 * This should be handled by the manager which evaluates the SIGCHLD signal.
 */
pid_t db_get_process(const char *projname, enum db_process_list_e list, enum db_process_state_e state)
{
    pid_t ret = -1;

    assert(state < PROCESS_STATE_MAX);
    assert(list < LIST_SELECTOR_MAX);

    struct qgis_project_list_s *projlist = NULL;
    switch (list)
    {
    case LIST_INIT:
    case LIST_ACTIVE:
    {
	projlist = projectlist;
	assert(projname);
	struct qgis_project_s *project = find_project_by_name(projlist, projname);
	if (project)
	{
	    struct qgis_process_list_s *proclist = qgis_project_get_process_list(project);
	    struct qgis_process_s *proc = qgis_process_list_mutex_find_process_by_status(proclist, state);
	    if (proc)
		ret = qgis_process_get_pid(proc);
	}
	break;
    }
    case LIST_SHUTDOWN:
#warning need to move shutdown list from shutdown module
	break;

    default:
	printlog("error: wrong list entry found %d", list);
	exit(EXIT_FAILURE);
    }

    return ret;
}


pid_t db_get_next_idle_process_for_work(const char *projname)
{
    pid_t ret = -1;

    assert(projname);
    struct qgis_project_s *project = find_project_by_name(projectlist, projname);
    if (project)
    {
	struct qgis_process_list_s *proclist = qgis_project_get_process_list(project);
	struct qgis_process_s *proc = qgis_process_list_find_idle_return_busy(proclist);
	if (proc)
	    ret = qgis_process_get_pid(proc);
    }

    return ret;
}


int db_get_process_socket(pid_t pid)
{
    int ret = -1;
    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    struct qgis_process_list_s *proc_list = qgis_project_get_process_list(project);
    struct qgis_process_s *proc = qgis_process_list_find_process_by_pid(proc_list, pid);
    if (proc)
	ret = qgis_process_get_socketfd(proc);

    return ret;
}


int db_process_set_state_idle(pid_t pid)
{
    int ret = -1;

    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    struct qgis_process_list_s *proc_list = qgis_project_get_process_list(project);
    struct qgis_process_s *proc = qgis_process_list_find_process_by_pid(proc_list, pid);
    if (proc)
	ret = qgis_process_set_state_idle(proc);

    return ret;
}


int db_get_num_process_by_status(const char *projname, enum db_process_state_e state)
{
    assert(projname);
    assert(state < PROCESS_STATE_MAX);

    int ret = -1;
    struct qgis_project_s *project = find_project_by_name(projectlist, projname);
    struct qgis_process_list_s *proc_list = qgis_project_get_process_list(project);
    ret = qgis_process_list_get_num_process_by_status(proc_list, state);

    return ret;
}


struct qgis_project_list_s *db_get_active_project_list(void)
{
    return projectlist;
}


