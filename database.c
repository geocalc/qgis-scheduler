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

#define _GNU_SOURCE

#include "database.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sqlite3.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <libgen.h>
#include <sys/queue.h>

#include "logger.h"
#include "qgis_shutdown_queue.h"
#include "statistic.h"


/* process data:
 * process id, list state, process state, worker thread id, socket fd
 *
 * project data:
 * project name, number of crashes during init phase
 */

#define DB_MAX_RETRIES	10


/* callback function for the common query function.
 * First parameter is the data given by "callback_arg"
 * from int db_select_parameter_callback(enum db_select_statement_id sid, sqlite3_callback callback, void *callback_arg, ...)
 * Second parameter is the number of columns in the result.
 * Third parameter is the data type of the result array, one entry for each result.
 * Forth parameter is the array of results, which have to be converted. Do like this:
 * switch (type[i])
 * {
 * case SQLITE_INTEGER:
 * 	int value = results[i].integer;
 * 	break;
 * case SQLITE_FLOAT:
 * 	double value = results[i].floating;
 * 	break;
 * case SQLITE_BLOB:
 * 	const void *data = results[i].blob;
 * 	break;
 * case SQLITE_NULL:
 * 	break;
 * case SQLITE_TEXT:
 * 	char *value = strdup(results[i].text);
 * 	break;
 * }
 * Fifth parameter is the array of column names.
 */
union callback_result_t {
    int integer;
    double floating;
    const unsigned char *text;
    const void *blob;
};
typedef int (*db_callback)(void *data, int ncol, int *type, union callback_result_t *results, const char**cols);


static sqlite3 *dbhandler = NULL;

/* transitional data. this will be deleted after the code change */
#include "qgis_project_list.h"

static struct qgis_project_list_s *projectlist = NULL;
static struct qgis_process_list_s *shutdownlist = NULL;	// list pf processes to be killed and removed
//static struct qgis_process_list_s *busylist = NULL;	// list of processes being state busy or added via api



enum db_select_statement_id
{
    DB_SELECT_ID_NULL = 0,
    DB_SELECT_CREATE_PROJECT_TABLE,
    DB_SELECT_CREATE_PROCESS_TABLE,
    // from this id on we can use prepared statements
    DB_SELECT_GET_NAMES_FROM_PROJECT,
    DB_INSERT_PROJECT_DATA,

    DB_SELECT_ID_MAX	// last entry, do not use
};


static const char *db_select_statement[DB_SELECT_ID_MAX] =
{
	// DB_SELECT_ID_NULL
	"",
	// DB_SELECT_CREATE_PROJECT_TABLE
	"CREATE TABLE projects (name TEXT UNIQ NOT NULL, configpath TEXT, configbasename TEXT, inotifyfd INTEGER, nr_crashs INTEGER)",
	// DB_SELECT_CREATE_PROCESS_TABLE
	"CREATE TABLE processes (projectname TEXT REFERENCES projects (name), "
	    "state INTEGER NOT NULL, threadid INTEGER, pid INTEGER UNIQ NOT NULL, "
	    "process_socket_fd INTEGER UNIQ NOT NULL, client_socket_fd INTEGER, "
	    "starttime_sec INTEGER, starttime_nsec INTEGER, signaltime_sec INTEGER, signaltime_nsec INTEGER )",
	// DB_SELECT_GET_NAMES_FROM_PROJECT
	"SELECT name FROM projects",
	// DB_INSERT_PROJECT_DATA
	"INSERT INTO projects (name, configpath, configbasename) VALUES (%s,%s,%s)",

};


static sqlite3_stmt *db_prepared_stmt[DB_SELECT_ID_MAX] = { 0 };


enum qgis_process_state_e db_state_to_qgis_state(enum db_process_state_e state)
{
    return (enum qgis_process_state_e)state;
}

enum db_process_state_e qgis_state_to_db_state(enum qgis_process_state_e state)
{
    return (enum db_process_state_e)state;
}


static sqlite3_stmt *db_statement_prepare(enum db_select_statement_id sid)
{
    assert(sid < DB_SELECT_ID_MAX);

    sqlite3_stmt *ppstmt;
    const char *const sql = db_select_statement[sid];
    const char *srcsql = sql;

    /* exchange "%N" markers with '?' */
    char * const copysql = strdup(sql);
    char *destsql = copysql;
    char c;
    do
    {
	c = *srcsql++;
	if ('%' == c)
	{
	    switch(*srcsql++)
	    {
	    case 'p':
		/* found pointer value "%p". */
		// fall through
	    case 'f':
		/* found double value "%f". */
		// fall through
	    case 's':
		/* found string value "%s". */
		// fall through
	    case 'd':
		/* found integer value "%i". */
		// fall through
	    case 'i':
		/* found integer value "%i". */

		/* exchange "%N" with '?' */
		c = '?';
		break;

	    case '%':
		/* found double percent sign "%%". just go on */

		/* exchange "%%" with '%' */
		break;

	    case '\0':
		/* percentage sign has been the last character in the string
		 * rewind the string, so the outer while() catches the end of
		 * the string.
		 */
		debug(1, "Huh? found '%%' at the end of the sql '%s'", sql);
		srcsql--;
		break;

	    default:
		/* unknown character found. exit */
		srcsql--;
		printlog("unknown character found in sql string '%s', position %ld: '%c'", db_select_statement[sid], (long int)(srcsql - db_select_statement[sid]), *srcsql);
		exit(EXIT_FAILURE);
	    }
	}

	*destsql++ = c;
    } while ( c ); // moved while() down here to copy terminating '\0' to "destsql"
    debug(1, "transferred sql from '%s' to '%s'", sql, copysql);

    int retval = sqlite3_prepare(dbhandler, copysql, -1, &ppstmt, NULL);
    if (SQLITE_OK != retval)
    {
	printlog("error: preparing sql statement '%s': %s", sql, sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }

    free(copysql);

    return ppstmt;
}


static void db_statement_finalize(sqlite3_stmt *ppstmt)
{
    int retval = sqlite3_finalize(ppstmt);
    if (SQLITE_OK != retval)
    {
	printlog("error: finalizing sql statement: %s", sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }
}


static int db_select_parameter_callback(enum db_select_statement_id sid, db_callback callback, void *callback_arg, ...)
{
    /* The life-cycle of a prepared statement object usually goes like this:
     *
     * 1. Create the prepared statement object using sqlite3_prepare_v2().
     * 2. Bind values to parameters using the sqlite3_bind_*() interfaces.
     * 3. Run the SQL by calling sqlite3_step() one or more times.
     * 4. Reset the prepared statement using sqlite3_reset() then go back to step 2. Do this zero or more times.
     * 5. Destroy the object using sqlite3_finalize().
     */
    int retval;

    assert(dbhandler);
    assert(sid < DB_SELECT_ID_MAX);
    const char *sql = db_select_statement[sid];
    debug(1, "db selected %d: '%s'", sid, sql);

    sqlite3_stmt *ppstmt;
    if ( !db_prepared_stmt[sid] )
	db_prepared_stmt[sid] =
		ppstmt = db_statement_prepare(sid);
    else
	ppstmt = db_prepared_stmt[sid];

    /* evaluate the remaining arguments */
    int col = 1;	// column position index
    va_list args;
    va_start(args, callback_arg);
    while (*sql)
    {
	if ('%' == *sql++)
	{
	    switch(*sql++)
	    {
	    case 'p':
		/* found pointer value "%p". The next argument is the
		 * type "void *" */
#warning todo
		assert(0);
		break;

	    case 'f':
		/* found double value "%f". The next argument is the
		 * type "double" */
#warning todo
		assert(0);
		break;

	    case 's':
		/* found string value "%s". The next argument is the
		 * type "const char *" */
	    {
                const char *s = va_arg(args, char *);
                retval = sqlite3_bind_text(ppstmt, col++, s, -1, SQLITE_STATIC);
                if ( SQLITE_OK != retval )
                {
                    printlog("error: in sql '%s' bind column %d returned: %s", sql, col, sqlite3_errstr(retval));
                    exit(EXIT_FAILURE);
                }
		break;
	    }

	    case 'd':	// fall through
	    case 'i':
		/* found integer value "%i". The next argument is the
		 * type "int" */
#warning todo
		assert(0);
		break;

	    case '%':
		/* found double percent sign "%%". just go on */
		break;

	    case '\0':
		/* percentage sign has been the last character in the string
		 * rewind the string, so the outer while() catches the end of
		 * the string.
		 */
		sql--;
		break;

	    default:
		/* unknown character found. exit */
		printlog("unknown character found in sql string '%s', position %ld: %c", db_select_statement[sid], (long int)(sql - db_select_statement[sid]), *sql);
		exit(EXIT_FAILURE);
	    }
	}
    }
    va_end(args);

    int try_num = 0;
    do {
	retval = sqlite3_step(ppstmt);
	if (SQLITE_BUSY == retval)
	    try_num++;
	else if (SQLITE_ROW == retval)
	{
	    /* there is data available, fetch data and recall step() */
	    debug(1, "data available: sql '%s'", db_select_statement[sid]);
	    assert(callback);
	    if ( !callback )
	    {
		printlog("error: data available but no callback function defined for sql '%s'", db_select_statement[sid]);
		/* go on with the loop until no more data is available */
	    }
	    else
	    {
		/* prepare the callback data */
		const int ncol_result = sqlite3_column_count(ppstmt);
		int *type = calloc(ncol_result, sizeof(*type));
		if ( !type )
		{
		    printlog("error: not enough memory");
		    exit(EXIT_FAILURE);
		}
		union callback_result_t *results = calloc(ncol_result, sizeof(*results));
		if ( !results )
		{
		    printlog("error: not enough memory");
		    exit(EXIT_FAILURE);
		}
		const char **cols = calloc(ncol_result, sizeof(*cols));
		if ( !cols )
		{
		    printlog("error: not enough memory");
		    exit(EXIT_FAILURE);
		}

		int i;
		for (i = 0; i<ncol_result; i++)
		{
		    int mytype =
			    type[i] = sqlite3_column_type(ppstmt, i);
		    switch(mytype)
		    {
		    case SQLITE_INTEGER:
			results[i].integer = sqlite3_column_int(ppstmt, i);
			break;

		    case SQLITE_FLOAT:
			results[i].floating = sqlite3_column_double(ppstmt, i);
			break;

		    case SQLITE_TEXT:
			results[i].text = sqlite3_column_text(ppstmt, i);
			break;

		    case SQLITE_BLOB:
			results[i].blob = sqlite3_column_blob(ppstmt, i);
			break;

		    case SQLITE_NULL:
			break;

		    default:
			printlog("error: unknown type %d", mytype);
			exit(EXIT_FAILURE);
		    }
		    cols[i] = sqlite3_column_name(ppstmt, i);
		}

		retval = callback(callback_arg, ncol_result, type, results, cols);

		//TODO: reuse arrays for the next row
		free(type);
		free(results);
		free(cols);

		if (retval)
		{
		    retval = SQLITE_ABORT;
		    break;
		}
	    }
	}
	else
	    break;
    } while (try_num < DB_MAX_RETRIES);

    switch(retval)
    {
    case SQLITE_BUSY:
	printlog("error: db busy! Exceeded max calls (%d) to fetch data", try_num);
	break;

    case SQLITE_ROW:
	/* error: there has been data available,
	 * but the program broke out of the loop?
	 */
	assert(0);
	break;

    case SQLITE_ERROR:
	/* there has been a data error. Print out and reset() the statement */
    {
	const char *sql = db_select_statement[sid];
	printlog("error: stepping sql statement '%s': %s", sql, sqlite3_errstr(retval));
	retval = sqlite3_reset(ppstmt);
	if (SQLITE_OK != retval)
	{
	    printlog("error: resetting sql statement '%s': %s", sql, sqlite3_errstr(retval));
	}
    }
    break;

    case SQLITE_MISUSE:
	/* the statement has been incorrect */
	printlog("error: misuse of prepared sql statement '%s'", db_select_statement[sid]);
	break;

    case SQLITE_ABORT:
	printlog("error: abort in callback function during steps of sql '%s'", db_select_statement[sid]);
	exit(EXIT_FAILURE);
	break;

    case SQLITE_OK:
    case SQLITE_DONE:
	/* the statement has finished successfully */
	retval = sqlite3_reset(ppstmt);
	if (SQLITE_OK != retval)
	{
	    const char *sql = db_select_statement[sid];
	    printlog("error: resetting sql statement '%s': %s", sql, sqlite3_errstr(retval));
	    exit(EXIT_FAILURE);
	}
	break;
    }

//    if ( !db_prepared_stmt[sid] )
//	db_statement_finalize(ppstmt);

    return 0;
}


#define db_select_parameter(sid, ...)	\
	db_select_parameter_callback(sid, NULL, NULL, # __VA_ARGS__ );


/* prepare database stements for use */
//static void db_statements_prepare(void)
//{
//    enum db_select_statement_id i;
//    for (i=DB_SELECT_GET_NAMES_FROM_PROJECT; i<DB_SELECT_ID_MAX; i++)
//    {
//	sqlite3_stmt *ppstmt = db_statement_prepare(i);
//	db_prepared_stmt[i] = ppstmt;
//    }
//}


/* delete prepared statements */
static void db_statements_finalize(void)
{
    enum db_select_statement_id i;
    for (i=DB_SELECT_ID_NULL; i<DB_SELECT_ID_MAX; i++)
    {
	sqlite3_stmt *ppstmt = db_prepared_stmt[i];
	db_statement_finalize(ppstmt);

	db_prepared_stmt[i] = NULL;
    }
}


/* called by sqlite3 function sqlite3_log()
 */
static void db_log(void *obsolete, int result, const char *msg)
{
    (void)obsolete;
    /* TODO documentation says we have to be thread save.
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
		"Can not set db log function", sqlite3_errstr(retval));
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
    db_select_parameter(DB_SELECT_CREATE_PROJECT_TABLE);

    db_select_parameter(DB_SELECT_CREATE_PROCESS_TABLE);

    /* prepare further statements */
//    db_statements_prepare();

    projectlist = qgis_proj_list_new();
    shutdownlist = qgis_process_list_new();
}


void db_delete(void)
{
    /* remove the projects */
    qgis_proj_list_delete(projectlist);
    qgis_process_list_delete(shutdownlist);


    debug(1, "shutdown memory db");
    db_statements_finalize();

    int retval = sqlite3_close(dbhandler);
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite3_close(): %s", sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }
    dbhandler = NULL;

    retval = sqlite3_shutdown();
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite3_shutdown(): %s", sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }
}


void db_add_project(const char *projname, const char *configpath)
{
    assert(projname);
    assert(configpath);

    char *basenam = basename(configpath);

    db_select_parameter_callback(DB_INSERT_PROJECT_DATA, NULL, NULL, projname, configpath, basenam);

    struct qgis_project_s *project = qgis_project_new(projname, configpath);
    qgis_proj_list_add_project(projectlist, project);

}


int db_get_names_project(char ***projname, int *len)
{
    assert(projname);
    assert(len);

    int retval = 0;
    int num = 0;
    char **array = NULL;
#if 1

    struct namelist_s
    {
	STAILQ_HEAD(listhead, nameiterator_s) head;	/* Linked list head */
    };

    struct nameiterator_s
    {
        STAILQ_ENTRY(nameiterator_s) entries;          /* Linked list prev./next entry */
        char *name;
    };

    int get_names_list(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	struct namelist_s *list = data;

	struct nameiterator_s *entry = malloc(sizeof(*entry));
	assert(entry);
	if ( !entry )
	{
	    logerror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	assert(1 == ncol);
	assert(SQLITE_TEXT == type[0]);
	entry->name = strdup((const char *)results[0].text);

	if (STAILQ_EMPTY(&list->head))
	    STAILQ_INSERT_HEAD(&list->head, entry, entries);
	else
	    STAILQ_INSERT_TAIL(&list->head, entry, entries);

	return 0;
    }


    struct namelist_s namelist;
    STAILQ_INIT(&namelist.head);

    db_select_parameter_callback(DB_SELECT_GET_NAMES_FROM_PROJECT, get_names_list, &namelist);

    struct nameiterator_s *it;
    STAILQ_FOREACH(it, &namelist.head, entries)
    {
	num++;
    }
    debug(1, "select found %d project names", num);

    /* aquire the pointer array */
    *len = num;
    array = calloc(num, sizeof(*array));
    *projname = array;

    num = 0;
    while( !STAILQ_EMPTY(&namelist.head) )
    {
	assert(num < *len);
	it = STAILQ_FIRST(&namelist.head);
	array[num++] = it->name;

	STAILQ_REMOVE_HEAD(&namelist.head, entries);
	free(it);
    }


#else
    int mylen;
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
    debug(1, "found %d project names", num);
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
#endif

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
    debug(1, "for process %d returned %d", pid, ret);

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
    debug(1, "for process %d returned %d", pid, ret);

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
    debug(1, "for process %d returned %d", pid, ret);

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
    debug(1, "for process %d returned %d", pid, ret);

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
    debug(1, "set state %d for process %d returned %d", state, pid, ret);

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


int db_get_list_process_by_list(pid_t **pidlist, int *len, enum db_process_list_e list)
{
    assert(pidlist);
    assert(len);
    assert(list < LIST_SELECTOR_MAX);

    struct pidlist_s
    {
	STAILQ_HEAD(listhead, piditerator_s) head;	/* Linked list head */
    };

    struct piditerator_s
    {
        STAILQ_ENTRY(piditerator_s) entries;          /* Linked list prev./next entry */
        pid_t pid;
    };

#if 0
    int get_pid_list(void *data, int ncol, char **text, char **cols)
    {
	struct pidlist_s *list = data;

	struct piditerator_s *entry = malloc(sizeof(*entry));
	assert(entry);
	if ( !entry )
	{
	    logerror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	entry->pid = atol(text[0]);	// TODO: get better converter or use converter from sqlite
		    // TODO is pid int or long int?

	if (STAILQ_EMPTY(&list->head))
	    STAILQ_INSERT_HEAD(&list->head, entry, entries);
	else
	    STAILQ_INSERT_TAIL(&list->head, entry, entries);

	return 0;
    }


    struct pidlist_s mypidlist;
    STAILQ_INIT(&mypidlist.head);

    static const char sql_get_pid[] = "SELECT name FROM projects WHERE list = %d";
    char *sql;
    int retval = asprintf(&sql, sql_get_pid, list);
    if (retval < 0)
    {
	logerror("error: asprintf");
	exit(EXIT_FAILURE);
    }

    char *errormsg;
    retval = sqlite3_exec(dbhandler, sql_get_pid, get_pid_list, &pidlist, &errormsg);
    if (SQLITE_OK != retval)
    {
	printlog("error: calling sqlite with '%s': %s", sql_get_pid, sqlite3_errstr(retval));
	exit(EXIT_FAILURE);
    }
    free(sql);
#else

    int retval = 0;
    struct pidlist_s mypidlist;
    STAILQ_INIT(&mypidlist.head);
    if (LIST_SHUTDOWN == list)
    {
	assert(shutdownlist);

	struct qgis_process_list_s *proclist = shutdownlist;
	struct qgis_process_iterator *process_iterator = qgis_process_list_get_iterator(proclist);
	while (process_iterator)
	{
	    struct qgis_process_s *process = qgis_process_list_get_next_process(&process_iterator);
	    pid_t pid = qgis_process_get_pid(process);

	    struct piditerator_s *entry = malloc(sizeof(*entry));
	    assert(entry);
	    if ( !entry )
	    {
		logerror("could not allocate memory");
		exit(EXIT_FAILURE);
	    }
	    entry->pid = pid;

	    if (STAILQ_EMPTY(&mypidlist.head))
		STAILQ_INSERT_HEAD(&mypidlist.head, entry, entries);
	    else
		STAILQ_INSERT_TAIL(&mypidlist.head, entry, entries);

	}
	qgis_process_list_return_iterator(proclist);
    }
    else
    {
	struct qgis_project_iterator *project_iterator = qgis_proj_list_get_iterator(projectlist);

	while (project_iterator)
	{
	    struct qgis_project_s *project = qgis_proj_list_get_next_project(&project_iterator);

	    struct qgis_process_list_s *proclist;
	    if (LIST_INIT == list)
		proclist = qgis_project_get_init_process_list(project);
	    else
		proclist = qgis_project_get_active_process_list(project);

	    struct qgis_process_iterator *process_iterator = qgis_process_list_get_iterator(proclist);
	    while (process_iterator)
	    {
		struct qgis_process_s *process = qgis_process_list_get_next_process(&process_iterator);
		pid_t pid = qgis_process_get_pid(process);

		struct piditerator_s *entry = malloc(sizeof(*entry));
		assert(entry);
		if ( !entry )
		{
		    logerror("could not allocate memory");
		    exit(EXIT_FAILURE);
		}
		entry->pid = pid;

		if (STAILQ_EMPTY(&mypidlist.head))
		    STAILQ_INSERT_HEAD(&mypidlist.head, entry, entries);
		else
		    STAILQ_INSERT_TAIL(&mypidlist.head, entry, entries);

	    }
	    qgis_process_list_return_iterator(proclist);

	}

	qgis_proj_list_return_iterator(projectlist);
    }

#endif

    int num = 0;
    struct piditerator_s *it;
    STAILQ_FOREACH(it, &mypidlist.head, entries)
    {
	num++;
    }
    debug(1, "select found %d processes", num);

    /* aquire the pointer array */
    *len = num;
    pid_t *array = calloc(num, sizeof(*array));
    *pidlist = array;

    num = 0;
    while( !STAILQ_EMPTY(&mypidlist.head) )
    {
	assert(num < *len);
	it = STAILQ_FIRST(&mypidlist.head);
	array[num++] = it->pid;

	STAILQ_REMOVE_HEAD(&mypidlist.head, entries);
	free(it);
    }

    return retval;
}


void db_free_list_process(pid_t *list, int len)
{
//    assert(list);
    free(list);
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
    debug(1, "project '%s'", projname);
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
    debug(1, "project '%s'", projname);
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
    debug(1, "for process %d returned %d", pid, ret);

    return ret;
}


int db_get_signal_timer(struct timespec *ts, pid_t pid)
{
    assert(ts);

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
    {
	*ts = *qgis_process_get_signaltime(proc);
	ret = 0;
    }
    debug(1, "pid %d, value %ld,%03lds. returned %d", pid, ts->tv_sec, (ts->tv_nsec/(1000*1000)), ret);

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
    debug(1, "removed %d processes from shutdown list", retval);

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


