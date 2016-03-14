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
#include <string.h>

#include "logger.h"
#include "qgis_shutdown_queue.h"
#include "statistic.h"


/* process data:
 * process id, list state, process state, worker thread id, socket fd
 *
 * project data:
 * project name, number of crashes during init phase
 */




static sqlite3 *dbhandler = NULL;

/* transitional data. this will be deleted after the code change */
#include "qgis_project_list.h"

static struct qgis_project_list_s *projectlist = NULL;
static struct qgis_process_list_s *shutdownlist = NULL;	// list pf processes to be killed and removed
//static struct qgis_process_list_s *busylist = NULL;	// list of processes being state busy or added via api




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
    static const char sql_project_table[] = "CREATE TABLE projects (name TEXT PRIMARY KEY NOT NULL, configpath TEXT, configbasename TEXT, inotifyfd INTEGER, nr_crashes INTEGER)";
    char *errormsg;
    retval = sqlite3_exec(dbhandler, sql_project_table, NULL, NULL, &errormsg);
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite with '%s': %s", sql_project_table, errormsg);
	exit(EXIT_FAILURE);
    }

    static const char sql_process_table[] = "CREATE TABLE processes (projectname TEXT REFERENCES projects (name), state INTEGER, threadid INTEGER, pid INTEGER, process_socket_fd INTEGER, client_socket_fd INTEGER, starttime_sec INTEGER, starttime_nsec INTEGER, signaltime_sec INTEGER, signaltime_nsec INTEGER )";
    retval = sqlite3_exec(dbhandler, sql_process_table, NULL, NULL, &errormsg);
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite with '%s': %s", sql_process_table, errormsg);
	exit(EXIT_FAILURE);
    }


    projectlist = qgis_proj_list_new();
    shutdownlist = qgis_process_list_new();


}


void db_shutdown(void)
{
    /* move the processes from the working lists to the shutdown module */
    struct qgis_project_iterator *proj_iterator = qgis_proj_list_get_iterator(projectlist);

    while (proj_iterator)
    {
	struct qgis_project_s *proj = qgis_proj_list_get_next_project(&proj_iterator);
	assert(proj);
	const char *projname = qgis_project_get_name(proj);
	assert(projname);

	db_move_all_process_from_init_to_shutdown_list(projname);
	db_move_all_process_from_active_to_shutdown_list(projname);
    }

    qgis_proj_list_return_iterator(projectlist);
}


void db_delete(void)
{
    /* remove the projects */
    qgis_proj_list_delete(projectlist);
    qgis_process_list_delete(shutdownlist);


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


void db_add_project(const char *projname, const char *configpath)
{
    assert(projname);
    assert(configpath);

    struct qgis_project_s *project = qgis_project_new(projname, configpath);
    qgis_proj_list_add_project(projectlist, project);

}


int db_get_names_project(char ***projname, int *len)
{
    assert(projname);
    assert(len);

    int retval = 0;
    int num = 0;
    int mylen;
    char **array;
    struct qgis_project_iterator *iterator;
//    if (!*projname || !*len)
    {
    iterator = qgis_proj_list_get_iterator(projectlist);
    while (iterator)
    {
	struct qgis_project_s *project = qgis_proj_list_get_next_project(&iterator);
	if (project)
	    num++;
    }
    qgis_proj_list_return_iterator(projectlist);
    /* aquire the pointer array */
    array = calloc(num, sizeof(char *));
    mylen = num;
    }
//    else
//    {
//	array = *projname;
//	mylen = *len;
//    }

    /* Note: inbetween these two calls the list may change.
     * We should solve this by copying all data into this function with one
     * call to qgis_proj_list_get_iterator() (instead of two)
     * But this is only a temporary solution, so...
     */
    num = 0;
    iterator = qgis_proj_list_get_iterator(projectlist);
    while (iterator)
    {
	if (num >= mylen)
	{
	    /* mehr einträge als platz zum speichern?
	     */
	    retval = -1;
	    break;
	}

	struct qgis_project_s *project = qgis_proj_list_get_next_project(&iterator);
	if (project)
	{
	    const char *name = qgis_project_get_name(project);
	    char *dup = strdup(name);
	    if ( !dup )
	    {
		logerror("error: can not allocate memory");
		exit(EXIT_FAILURE);
	    }
	    array[num] = dup;
	    num++;
	}
    }
    qgis_proj_list_return_iterator(projectlist);

    *projname = array;
    *len = mylen;

    return retval;
}


void db_free_names_project(char **projname, int len)
{
    assert(projname);
    assert(len >= 0);

    int i;
    for (i=0; i<len; i++)
    {
	free(projname[i]);
    }
    free(projname);
}


void db_add_process(const char *projname, pid_t pid, int process_socket_fd)
{
    assert(projname);
    assert(pid > 0);
    assert(process_socket_fd >= 0);

    struct qgis_process_s *childproc = qgis_process_new(pid, process_socket_fd);
    struct qgis_project_s *project = find_project_by_name(projectlist, projname );
    qgis_project_add_process(project, childproc);
}


//int db_get_num_idle_process(const char *projname);


const char *db_get_project_for_this_process(pid_t pid)
{
    const char *ret = NULL;

    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);

    if (project)
	ret = qgis_project_get_name(project);

    return ret;
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
	    struct qgis_process_list_s *proclist = qgis_project_get_active_process_list(project);
	    assert(proclist);
	    struct qgis_process_s *proc = qgis_process_list_mutex_find_process_by_status(proclist, state);
	    if (proc)
		ret = qgis_process_get_pid(proc);
	}
	break;
    }
    case LIST_SHUTDOWN:
	assert(0);
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
	struct qgis_process_list_s *proclist = qgis_project_get_active_process_list(project);
	assert(proclist);
	struct qgis_process_s *proc = qgis_process_list_find_idle_return_busy(proclist);
	if (proc)
	    ret = qgis_process_get_pid(proc);
    }

    return ret;
}


/* return 0 if the pid is not in any of the process lists, 1 otherwise */
int db_has_process(pid_t pid)
{
    int ret = 0;

    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    if (project)
    {
	ret = 1;
    }
    else
    {
	struct qgis_process_s *proc = qgis_process_list_find_process_by_pid(shutdownlist, pid);
	if (proc)
	    ret = 1;
    }

    return ret;
}


int db_get_process_socket(pid_t pid)
{
    int ret = -1;
    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    if (project)
    {
	struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	assert(proc_list);
	struct qgis_process_s *proc = qgis_process_list_find_process_by_pid(proc_list, pid);
	if (proc)
	{
	    ret = qgis_process_get_socketfd(proc);
	}
	else
	{
	    proc_list = qgis_project_get_init_process_list(project);
	    assert(proc_list);
	    struct qgis_process_s *proc = qgis_process_list_find_process_by_pid(proc_list, pid);
	    if (proc)
	    {
		ret = qgis_process_get_socketfd(proc);
	    }
	}
    }

    return ret;
}


enum db_process_state_e db_get_process_state(pid_t pid)
{
    enum db_process_state_e ret = PROCESS_STATE_MAX ;
    struct qgis_process_s *proc = NULL;
    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    if (project)
    {
	struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	assert(proc_list);
	proc = qgis_process_list_find_process_by_pid(proc_list, pid);
    }
    else
    {
	proc = qgis_process_list_find_process_by_pid(shutdownlist, pid);
    }
    if (proc)
	ret = qgis_process_get_state(proc);

    return ret;
}


int db_process_set_state_init(pid_t pid, pthread_t thread_id)
{
    int ret = -1;
    struct qgis_process_s *proc = NULL;
    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    if (project)
    {
	struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	assert(proc_list);
	proc = qgis_process_list_find_process_by_pid(proc_list, pid);
	if (!proc)
	{
	    proc_list = qgis_project_get_init_process_list(project);
	    assert(proc_list);
	    proc = qgis_process_list_find_process_by_pid(proc_list, pid);
	}
    }
    /* no need to test the shutdown list, processes in there won't get the
     * state "init".
     */
//    else
//    {
//	proc = qgis_process_list_find_process_by_pid(shutdownlist, pid);
//    }
    if (proc)
	ret = qgis_process_set_state_init(proc, thread_id);

    return ret;

}


int db_process_set_state_idle(pid_t pid)
{
    int ret = -1;
    struct qgis_process_s *proc = NULL;
    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    if (project)
    {
	struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	assert(proc_list);
	proc = qgis_process_list_find_process_by_pid(proc_list, pid);
	if (!proc)
	{
	    proc_list = qgis_project_get_init_process_list(project);
	    assert(proc_list);
	    proc = qgis_process_list_find_process_by_pid(proc_list, pid);
	}
    }
    else
    {
	proc = qgis_process_list_find_process_by_pid(shutdownlist, pid);
    }
    if (proc)
	ret = qgis_process_set_state_idle(proc);

    return ret;
}


int db_process_set_state_exit(pid_t pid)
{
    int ret = -1;
    struct qgis_process_s *proc = NULL;
    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    if (project)
    {
	struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	assert(proc_list);
	proc = qgis_process_list_find_process_by_pid(proc_list, pid);
    }
    else
    {
	proc = qgis_process_list_find_process_by_pid(shutdownlist, pid);
    }
    if (proc)
	ret = qgis_process_set_state_exit(proc);

    return ret;
}


int db_process_set_state(pid_t pid, enum db_process_state_e state)
{
    int ret = -1;
    struct qgis_process_s *proc = NULL;
    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    if (project)
    {
	struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	assert(proc_list);
	proc = qgis_process_list_find_process_by_pid(proc_list, pid);
    }
    else
    {
	proc = qgis_process_list_find_process_by_pid(shutdownlist, pid);
    }
    if (proc)
	ret = qgis_process_set_state(proc, state);

    return ret;
}


int db_get_num_process_by_status(const char *projname, enum db_process_state_e state)
{
    assert(projname);
    assert(state < PROCESS_STATE_MAX);

    int ret = -1;
    struct qgis_project_s *project = find_project_by_name(projectlist, projname);
    if (project)
    {
	struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	assert(proc_list);
	ret = qgis_process_list_get_num_process_by_status(proc_list, state);
    }
    else
    {
	ret = qgis_process_list_get_num_process_by_status(shutdownlist, state);
    }

    return ret;
}


/* return the number of processes being in the active list of this project */
int db_get_num_active_process(const char *projname)
{
    assert(projname);

    int ret = -1;
    struct qgis_project_s *project = find_project_by_name(projectlist, projname);
    if (project)
    {
	struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	assert(proc_list);
	ret = qgis_process_list_get_num_process(proc_list);
    }

    return ret;
}


void db_move_process_to_list(enum db_process_list_e list, pid_t pid)
{
    struct qgis_process_s *proc;
    switch(list)
    {
    // TODO: separate init and active processes in separate lists
    case LIST_INIT:
    case LIST_ACTIVE:
	/* nothing to do.
	 * only check if that process is in shutdown list already and error out
	 */
//	proc = qgis_process_list_find_process_by_pid(busylist, pid);
//	if (proc)
//	{
//	    printlog("error: shall not move process %d from shutdown list to active list", pid);
//	    exit(EXIT_FAILURE);
//	}
	proc = qgis_process_list_find_process_by_pid(shutdownlist, pid);
	if (proc)
	{
	    printlog("error: shall not move process %d from shutdown list to active list", pid);
	    exit(EXIT_FAILURE);
	}
	break;

    case LIST_SHUTDOWN:
    {
	struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
	if (project)
	{
	    struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	    proc = qgis_process_list_find_process_by_pid(proc_list, pid);
	    if (!proc)
	    {
		debug(1, "error: did not find process %d in active list", pid);
	    }
	    else
	    {
		qgis_process_list_transfer_process(shutdownlist, proc_list, proc);
	    }
	}
	else
	{
	    debug(1, "error: did not find process %d in projects", pid);
	}
	break;
    }
    default:
	printlog("error: unknown list enumeration %d", list);
	exit(EXIT_FAILURE);
    }
}


enum db_process_list_e db_get_process_list(pid_t pid)
{
    enum db_process_list_e ret = LIST_SELECTOR_MAX;
    struct qgis_process_s *proc = NULL;
    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    if (project)
    {
	struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	assert(proc_list);
	proc = qgis_process_list_find_process_by_pid(proc_list, pid);
	if (proc)
	{
	    ret = LIST_ACTIVE;
	}
	else
	{
	    proc_list = qgis_project_get_init_process_list(project);
	    assert(proc_list);
	    proc = qgis_process_list_find_process_by_pid(proc_list, pid);
	    if (proc)
		ret = LIST_INIT;
	}
    }
    else
    {
	proc = qgis_process_list_find_process_by_pid(shutdownlist, pid);
	if (proc)
	    ret = LIST_SHUTDOWN;
    }

    return ret;
}


/* processes in the init list with state idle are done with the initialization.
 * move these processes to the active list to be picked up for net responses.
 */
void db_move_all_idle_process_from_init_to_active_list(const char *projname)
{
    struct qgis_project_s *project = find_project_by_name(projectlist, projname );
    struct qgis_process_list_s *activeproclist = qgis_project_get_active_process_list(project);
    struct qgis_process_list_s *initproclist = qgis_project_get_init_process_list(project);

    int retval = qgis_process_list_transfer_all_process_with_state(activeproclist, initproclist, PROC_IDLE);
    debug(1, "project '%s' moved %d processes from init list to active list", projname, retval);

}


/* move all processes from the active list to the shutdown list to be deleted
 */
void db_move_all_process_from_active_to_shutdown_list(const char *projname)
{
    struct qgis_project_s *project = find_project_by_name(projectlist, projname );
    struct qgis_process_list_s *proclist = qgis_project_get_active_process_list(project);
    int shutdownnum = qgis_process_list_get_num_process(proclist);
    statistic_add_process_shutdown(shutdownnum);
    qgis_process_list_transfer_all_process( shutdownlist, proclist );
    qgis_shutdown_notify_changes();
}


/* move all processes from the init list to the shutdown list to be deleted
 */
void db_move_all_process_from_init_to_shutdown_list(const char *projname)
{
    struct qgis_project_s *project = find_project_by_name(projectlist, projname );
    struct qgis_process_list_s *proclist = qgis_project_get_init_process_list(project);
    int shutdownnum = qgis_process_list_get_num_process(proclist);
    statistic_add_process_shutdown(shutdownnum);
    qgis_process_list_transfer_all_process( shutdownlist, proclist );
    qgis_shutdown_notify_changes();
}


/* returns the next process (pid) which needs to be worked on.
 * This could be
 * (1) a process which is transferred from busy to idle state and needs a TERM signal
 * (2) a process which is not removed from RAM and needs a KILL signal after a timeout
 * (3) a process which is not removed from RAM after a timeout and need to be removed from the db
 */
pid_t db_get_shutdown_process_in_timeout(void)
{
    pid_t ret = -1;

    struct qgis_process_s *proc = get_next_shutdown_proc(shutdownlist);
    if (proc)
    {
	ret = qgis_process_get_pid(proc);
    }

    return ret;
}


int db_reset_signal_timer(pid_t pid)
{
    int ret = -1;
    struct qgis_process_s *proc = NULL;
    struct qgis_project_s *project = qgis_proj_list_find_project_by_pid(projectlist, pid);
    if (project)
    {
	struct qgis_process_list_s *proc_list = qgis_project_get_active_process_list(project);
	assert(proc_list);
	proc = qgis_process_list_find_process_by_pid(proc_list, pid);
    }
    else
    {
	proc = qgis_process_list_find_process_by_pid(shutdownlist, pid);
    }
    if (proc)
	ret = qgis_process_reset_signaltime(proc);

    return ret;
}


void db_shutdown_get_min_signaltimer(struct timespec *maxtimeval)
{
    qgis_process_list_get_min_signaltimer(shutdownlist, maxtimeval);
}


int db_get_num_shutdown_processes(void)
{
    int num_list = qgis_process_list_get_num_process(shutdownlist);

    return num_list;
}


int db_remove_process_with_state_exit(void)
{
    int retval = qgis_process_list_delete_all_process_with_state(shutdownlist, PROC_EXIT);

    return retval;
}


void db_inc_startup_failures(const char *projname)
{
    assert(projname);

    struct qgis_project_s *project = find_project_by_name(projectlist, projname);
    if (project)
    {
	qgis_project_inc_nr_crashes(project);
    }
}


int db_get_startup_failures(const char *projname)
{
    assert(projname);

    int ret = -1;
    struct qgis_project_s *project = find_project_by_name(projectlist, projname);
    if (project)
    {
	ret = qgis_project_get_nr_crashes(project);
    }

    return ret;
}


void db_reset_startup_failures(const char *projname)
{
    assert(projname);

    struct qgis_project_s *project = find_project_by_name(projectlist, projname);
    if (project)
    {
	qgis_project_reset_nr_crashes(project);	// reset number of crashes after configuration change
    }

}


const char *db_get_project_for_watchid(int watchid)
{
    assert(watchid >= 0);

    const char *ret = NULL;

    struct qgis_project_s *project = qgis_proj_list_find_project_by_inotifyid(projectlist, watchid);
    if (project)
	ret = qgis_project_get_name(project);

    return ret;
}


