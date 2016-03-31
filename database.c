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
#include <errno.h>

#include "logger.h"
#include "qgis_config.h"
#include "qgis_shutdown_queue.h"
#include "statistic.h"
#include "timer.h"


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
    long long int integer;
    double floating;
    const unsigned char *text;
    const void *blob;
};
typedef int (*db_callback)(void *data, int ncol, int *type, union callback_result_t *results, const char**cols);


static sqlite3 *dbhandler = NULL;

/* transitional data. this will be deleted after the code change */
#include "qgis_project_list.h"

static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;



enum db_select_statement_id
{
    DB_SELECT_ID_NULL = 0,
    // from this id on we can use prepared statements
    DB_SELECT_CREATE_PROJECT_TABLE,
    DB_SELECT_CREATE_PROCESS_TABLE,
    DB_SELECT_GET_NAMES_FROM_PROJECT,
    DB_INSERT_PROJECT_DATA,
    DB_INSERT_PROCESS_DATA,
    DB_UPDATE_PROCESS_STATE,
    DB_GET_PROCESS_STATE,
    DB_GET_STATE_PROCESS,
    DB_GET_PROCESS_FROM_LIST,
    DB_GET_NUM_PROCESS_FROM_LIST,
    DB_UPDATE_PROCESS_LISTS_WITH_NAME_AND_LIST,
    DB_UPDATE_PROCESS_LIST_PID,
    DB_UPDATE_PROCESS_LIST,
    DB_UPDATE_PROCESS_SIGNAL_TIMER,
    DB_SELECT_PROCESS_SIGNAL_TIMER,
    DB_SELECT_PROCESS_MIN_SIGNAL_TIMER,
    DB_DELETE_PROCESS_WITH_STATE,
    DB_INC_PROJECT_STARTUP_FAILURE,
    DB_SELECT_PROJECT_STARTUP_FAILURE,
    DB_RESET_PROJECT_STARTUP_FAILURE,
    DB_SELECT_PROJECT_WITH_PID,
    DB_SELECT_PROCESS_WITH_NAME_LIST_AND_STATE,
    DB_GET_LIST_FROM_PROCESS,
    DB_GET_PROCESS_SOCKET_FROM_PROCESS,
    DB_GET_PROJECT_FROM_WATCHID,
    DB_DUMP_PROJECT,
    DB_DUMP_PROCESS,

    DB_SELECT_ID_MAX	// last entry, do not use
};


static const char *db_select_statement[DB_SELECT_ID_MAX] =
{
	// DB_SELECT_ID_NULL
	"",
	// DB_SELECT_CREATE_PROJECT_TABLE
	"CREATE TABLE projects (name TEXT UNIQ NOT NULL, configpath TEXT, configbasename TEXT, inotifyfd INTEGER, nr_crashs INTEGER DEFAULT 0)",
	// DB_SELECT_CREATE_PROCESS_TABLE
	"CREATE TABLE processes (projectname TEXT REFERENCES projects (name), "
	    "list INTEGER NOT NULL, state INTEGER NOT NULL, "
	    "threadid INTEGER, pid INTEGER UNIQ NOT NULL, "
	    "process_socket_fd INTEGER UNIQ NOT NULL, client_socket_fd INTEGER DEFAULT -1, "
	    "starttime_sec INTEGER DEFAULT 0, starttime_nsec INTEGER DEFAULT 0, "
	    "signaltime_sec INTEGER DEFAULT 0, signaltime_nsec INTEGER DEFAULT 0 )",
	// DB_SELECT_GET_NAMES_FROM_PROJECT
	"SELECT name FROM projects",
	// DB_INSERT_PROJECT_DATA
	"INSERT INTO projects (name, configpath, configbasename, inotifyfd) VALUES (%s,%s,%s,%i)",
	// DB_INSERT_PROCESS_DATA
	"INSERT INTO processes (projectname, list, state, pid, process_socket_fd) VALUES (%s,%i,%i,%i,%i)",
	// DB_UPDATE_PROCESS_STATE
	"UPDATE processes SET state = %i, threadid = %l WHERE pid = %i",
	// DB_GET_PROCESS_STATE
	"SELECT state FROM processes WHERE pid = %i",
	// DB_GET_STATE_PROCESS
	"SELECT pid FROM processes WHERE projectname = %s AND state = %i",
	// DB_GET_PROCESS_FROM_LIST
	"SELECT pid FROM processes WHERE list = %d",
	// DB_GET_NUM_PROCESS_FROM_LIST
	"SELECT count(pid) FROM processes WHERE list = %d",
	// DB_UPDATE_PROCESS_LISTS_WITH_NAME_AND_LIST
	"UPDATE processes SET list = %i WHERE projectname = %s AND list = %i",
	// DB_UPDATE_PROCESS_LIST_PID
	"UPDATE processes SET list = %i WHERE pid = %i",
	// DB_UPDATE_PROCESS_LIST
	"UPDATE processes SET list = %i",
	// DB_UPDATE_PROCESS_SIGNAL_TIMER
	"UPDATE processes SET signaltime_sec = %l, signaltime_nsec = %l WHERE pid = %i",
	// DB_SELECT_PROCESS_SIGNAL_TIMER
	"SELECT signaltime_sec,signaltime_nsec FROM processes WHERE pid = %i",
	// DB_SELECT_PROCESS_MIN_SIGNAL_TIMER
	"SELECT signaltime_sec,signaltime_nsec FROM processes WHERE signaltime_sec != 0  AND signaltime_nsec != 0 ORDER BY signaltime_sec ASC, signaltime_nsec ASC LIMIT 1",
	// DB_DELETE_PROCESS_WITH_STATE
	"DELETE FROM processes WHERE STATE = %i",
	// DB_INC_PROJECT_STARTUP_FAILURE
	"UPDATE projects SET nr_crashs = nr_crashs+1 WHERE name = %s",
	// DB_SELECT_PROJECT_STARTUP_FAILURE
	"SELECT nr_crashs FROM projects WHERE name = %s",
	// DB_RESET_PROJECT_STARTUP_FAILURE
	"UPDATE projects SET nr_crashs = 0 WHERE name = %s",
	// DB_SELECT_PROJECT_WITH_PID
	"SELECT projectname FROM processes WHERE pid = %i",
	// DB_SELECT_PROCESS_WITH_NAME_LIST_AND_STATE
	"SELECT pid FROM processes WHERE (projectname= %s AND list = %i AND state = %i) LIMIT 1",
	// DB_GET_LIST_FROM_PROCESS
	"SELECT list FROM processes WHERE pid = %d",
	// DB_GET_PROCESS_SOCKET_FROM_PROCESS
	"SELECT process_socket_fd FROM processes WHERE pid = %d",
	// DB_GET_PROJECT_FROM_WATCHID
	"SELECT name FROM projects WHERE inotifyfd = %d",
	// DB_DUMP_PROJECT
	"SELECT * FROM projects",
	// DB_DUMP_PROCESS
	"SELECT * FROM processes",


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
		// fall through
	    case 'l':
		/* found long long integer value "%l". */

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
#define BUFFERSIZE 32
    int retval;
    char debug_buffer[BUFFERSIZE] = {0,};
    const int loglevel = config_get_debuglevel();

    assert(dbhandler);
    assert(sid < DB_SELECT_ID_MAX);
    const char *sql = db_select_statement[sid];

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
	    {
		assert(0); // TODO need to extend "%p" to "%NNNp" with NNN being decimal number describing the size of 'p'
                const void *v = va_arg(args, void *);
                retval = sqlite3_bind_blob(ppstmt, col++, v, -1, SQLITE_STATIC);
                if ( SQLITE_OK != retval )
                {
                    printlog("error: in sql '%s' bind column %d returned: %s", sql, col, sqlite3_errstr(retval));
                    exit(EXIT_FAILURE);
                }
                if (1 <= loglevel)
                    snprintf(debug_buffer+strlen(debug_buffer), BUFFERSIZE-strlen(debug_buffer)-1, ", %p", v);
		break;
	    }

	    case 'f':
		/* found double value "%f". The next argument is the
		 * type "double" */
	    {
                double d = va_arg(args, double);
                retval = sqlite3_bind_double(ppstmt, col++, d);
                if ( SQLITE_OK != retval )
                {
                    printlog("error: in sql '%s' bind column %d returned: %s", sql, col, sqlite3_errstr(retval));
                    exit(EXIT_FAILURE);
                }
                if (1 <= loglevel)
                    snprintf(debug_buffer+strlen(debug_buffer), BUFFERSIZE-strlen(debug_buffer)-1, ", %f", d);
		break;
	    }

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
                if (1 <= loglevel)
                    snprintf(debug_buffer+strlen(debug_buffer), BUFFERSIZE-strlen(debug_buffer)-1, ", %s", s);
		break;
	    }

	    case 'd':	// fall through
	    case 'i':
		/* found integer value "%i". The next argument is the
		 * type "int" */
	    {
                int i = va_arg(args, int);
                retval = sqlite3_bind_int(ppstmt, col++, i);
                if ( SQLITE_OK != retval )
                {
                    printlog("error: in sql '%s' bind column %d returned: %s", sql, col, sqlite3_errstr(retval));
                    exit(EXIT_FAILURE);
                }
                if (1 <= loglevel)
                    snprintf(debug_buffer+strlen(debug_buffer), BUFFERSIZE-strlen(debug_buffer)-1, ", %d", i);
		break;
	    }

	    case 'l':
		/* found 64bit integer value "%l". The next argument is the
		 * type "long long int" */
	    {
                long long int l = va_arg(args, long long int);
                retval = sqlite3_bind_int64(ppstmt, col++, l);
                if ( SQLITE_OK != retval )
                {
                    printlog("error: in sql '%s' bind column %d returned: %s", sql, col, sqlite3_errstr(retval));
                    exit(EXIT_FAILURE);
                }
                if (1 <= loglevel)
                    snprintf(debug_buffer+strlen(debug_buffer), BUFFERSIZE-strlen(debug_buffer)-1, ", %lld", l);
		break;
	    }

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
    debug_buffer[BUFFERSIZE-1] = '\0';
    debug(1, "db selected %d: '%s%s'", sid, db_select_statement[sid], debug_buffer);

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
			results[i].integer = sqlite3_column_int64(ppstmt, i);
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
	db_select_parameter_callback(sid, NULL, NULL, ##__VA_ARGS__);


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


static pid_t db_nolock__get_process(const char *projname, enum db_process_list_e list, enum db_process_state_e state)
{
    assert(state < PROCESS_STATE_MAX);
    assert(list < LIST_SELECTOR_MAX);

    int get_process(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	int *proc = data;

	assert(1 == ncol);
	assert(SQLITE_INTEGER == type[0]);

	*proc = results[0].integer;

	return 0;
    }

    pid_t ret = -1;
    int mylist = list;
    int mystate = state;

    db_select_parameter_callback(DB_SELECT_PROCESS_WITH_NAME_LIST_AND_STATE, get_process, &ret, projname, mylist, mystate);

    debug(1, "returned %d", ret);

    return ret;
}


static int db_nolock__process_set_state(pid_t pid, enum db_process_state_e state, pthread_t threadid)
{
    int ret = 0;

    // we need to type cast values to a "guaranteed" 64 bit value
    // because the vararg parser assumes type "long long int" with "%l"
    int mystate = state;
    int mypid = pid;
    long long int mythreadid = threadid;
    db_select_parameter(DB_UPDATE_PROCESS_STATE, mystate, mythreadid, mypid);

    return ret;
}



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


//    debug(1, "db mutex init at %p", &db_lock);

    /* setup all tables */
    db_select_parameter(DB_SELECT_CREATE_PROJECT_TABLE);

    db_select_parameter(DB_SELECT_CREATE_PROCESS_TABLE);

    /* prepare further statements */
//    db_statements_prepare();
}


void db_delete(void)
{
    /* remove the projects */

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


void db_add_project(const char *projname, const char *configpath, int inotifyfd)
{
    assert(projname);
    assert(configpath);

    char *basenam = basename(configpath);

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_INSERT_PROJECT_DATA, projname, configpath, basenam, inotifyfd);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }
}


int db_get_names_project(char ***projname, int *len)
{
    assert(projname);
    assert(len);

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

    int ret = 0;
    int num = 0;
    char **array = NULL;

    struct namelist_s namelist;
    STAILQ_INIT(&namelist.head);

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_SELECT_GET_NAMES_FROM_PROJECT, get_names_list, &namelist);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

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


    return ret;
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

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_INSERT_PROCESS_DATA, projname, LIST_INIT, PROC_STATE_START, pid, process_socket_fd);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }
}


//int db_get_num_idle_process(const char *projname);


char *db_get_project_for_this_process(pid_t pid)
{
    int get_projectname(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	const char **projname = data;

	assert(1 == ncol);
	assert(SQLITE_TEXT == type[0]);

	*projname = strdup((const char *)results[0].text);

	return 0;
    }

    char *ret = NULL;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_SELECT_PROJECT_WITH_PID, get_projectname, &ret, pid);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    debug(1, "returned %s", ret);

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
    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    pid_t ret = db_nolock__get_process(projname, list, state);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


pid_t db_get_next_idle_process_for_busy_work(const char *projname)
{
    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    pid_t ret = db_nolock__get_process(projname, LIST_ACTIVE, PROC_STATE_IDLE);

    if (0 < ret)
	db_nolock__process_set_state(ret, PROC_STATE_BUSY, 0);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


/* return 0 if the pid is not in any of the process lists, 1 otherwise */
int db_has_process(pid_t pid)
{

    int has_process(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	int *val = data;

	assert(1 == ncol);
	assert(SQLITE_INTEGER == type[0]);
	*val = 1;

	return 0;
    }

    int ret = 0;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_GET_PROCESS_STATE, has_process, &ret, (int)pid);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    debug(1, "pid = %d returned %d", pid, ret);

    return ret;
}


int db_get_process_socket(pid_t pid)
{

    int get_process_socket(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	int *socket = data;

	assert(1 == ncol);
	assert(SQLITE_INTEGER == type[0]);
	*socket = results[0].integer;

	return 0;
    }

    int ret = -1;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_GET_PROCESS_SOCKET_FROM_PROCESS, get_process_socket, &ret, (int)pid);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


enum db_process_state_e db_get_process_state(pid_t pid)
{

    int get_process_state(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	enum db_process_state_e *state = data;

	assert(1 == ncol);
	assert(SQLITE_INTEGER == type[0]);
	*state = results[0].integer;

	return 0;
    }

    enum db_process_state_e ret = PROCESS_STATE_MAX ;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_GET_PROCESS_STATE, get_process_state, &ret, pid);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    debug(1, "for process %d returned %d", pid, ret);

    return ret;
}


int db_process_set_state_init(pid_t pid, pthread_t thread_id)
{
    int ret = 0;

    // we need to copy values to a "guaranteed" 64 bit value
    // because the vararg parser assumes type "long long int" with "%l"
    long long threadid = thread_id;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_nolock__process_set_state(pid, PROC_STATE_INIT, threadid);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;

}


int db_process_set_state_idle(pid_t pid)
{
    int ret = 0;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_nolock__process_set_state(pid, PROC_STATE_IDLE, 0);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


int db_process_set_state_exit(pid_t pid)
{
    int ret = 0;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_nolock__process_set_state(pid, PROC_STATE_EXIT, 0);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


int db_process_set_state(pid_t pid, enum db_process_state_e state)
{
    int ret = 0;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_nolock__process_set_state(pid, state, 0);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


int db_get_num_process_by_status(const char *projname, enum db_process_state_e state)
{
    assert(projname);
    assert(state < PROCESS_STATE_MAX);

    int get_pid_by_status(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	int *num = data;

	assert(1 == ncol);
	assert(SQLITE_INTEGER == type[0]);

	(*num)++;

	return 0;
    }

    int ret = 0;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_GET_STATE_PROCESS, get_pid_by_status, &ret, projname, (int)state);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    debug(1, "returned %d", ret);

    return ret;
}


/* return the number of processes being in the active list of this project */
int db_get_num_active_process(const char *projname)
{
    assert(projname);

    /* intentional no lock here,
     * Lock aquired in db_get_num_process_by_status()
     */
    int ret = db_get_num_process_by_status(projname, PROC_STATE_BUSY);

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


    int get_pid_list(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	struct pidlist_s *list = data;

	struct piditerator_s *entry = malloc(sizeof(*entry));
	assert(entry);
	if ( !entry )
	{
	    logerror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}

	assert(1 == ncol);
	assert(SQLITE_INTEGER == type[0]);
	entry->pid = results[0].integer;

	if (STAILQ_EMPTY(&list->head))
	    STAILQ_INSERT_HEAD(&list->head, entry, entries);
	else
	    STAILQ_INSERT_TAIL(&list->head, entry, entries);

	return 0;
    }


    struct pidlist_s mypidlist;
    STAILQ_INIT(&mypidlist.head);

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_GET_PROCESS_FROM_LIST, get_pid_list, &mypidlist, (int)list);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    int ret = 0;
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

    return ret;
}


void db_free_list_process(pid_t *list, int len)
{
//    assert(list);
    free(list);
}


void db_move_process_to_list(enum db_process_list_e list, pid_t pid)
{
    assert(LIST_SELECTOR_MAX > list);
    assert(0 < pid);

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_UPDATE_PROCESS_LIST_PID, list, pid);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }
}


enum db_process_list_e db_get_process_list(pid_t pid)
{
    assert(0 < pid);

    int get_list_pid(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	enum db_process_list_e *list = data;

	assert(1 == ncol);
	assert(SQLITE_INTEGER == type[0]);
	*list = results[0].integer;

	return 0;
    }

    enum db_process_list_e ret = LIST_SELECTOR_MAX;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_GET_LIST_FROM_PROCESS, get_list_pid, &ret, (int)pid);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    debug(1, "returned %d", ret);

    return ret;
}


/* processes in the init list with state idle are done with the initialization.
 * move these processes to the active list to be picked up for net responses.
 */
void db_move_all_idle_process_from_init_to_active_list(const char *projname)
{
    debug(1, "project '%s'", projname);

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_UPDATE_PROCESS_LISTS_WITH_NAME_AND_LIST, LIST_ACTIVE, projname, LIST_INIT);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    qgis_shutdown_notify_changes();
}


/* move all processes from the active list to the shutdown list to be deleted
 */
void db_move_all_process_from_active_to_shutdown_list(const char *projname)
{
    debug(1, "project '%s'", projname);

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_UPDATE_PROCESS_LISTS_WITH_NAME_AND_LIST, LIST_SHUTDOWN, projname, LIST_ACTIVE);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    qgis_shutdown_notify_changes();
}


/* move all processes from the init list to the shutdown list to be deleted
 */
void db_move_all_process_from_init_to_shutdown_list(const char *projname)
{
    debug(1, "project '%s'", projname);

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_UPDATE_PROCESS_LISTS_WITH_NAME_AND_LIST, LIST_SHUTDOWN, projname, LIST_INIT);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    qgis_shutdown_notify_changes();
}


void db_move_all_process_to_list(enum db_process_list_e list)
{
    assert(LIST_SELECTOR_MAX > list);

    int mylist = list;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_UPDATE_PROCESS_LIST, mylist);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }
}

/* returns the next process (pid) which needs to be worked on.
 * This could be
 * (1) a process which is transferred from busy to idle state and needs a TERM signal
 * (2) a process which is not removed from RAM and needs a KILL signal after a timeout
 * (3) a process which is not removed from RAM after a timeout and need to be removed from the db
 */
//pid_t db_get_shutdown_process_in_timeout(void)
//{
//    pid_t ret = -1;
//
//    struct qgis_process_s *proc = get_next_shutdown_proc(shutdownlist);
//    if (proc)
//    {
//	ret = qgis_process_get_pid(proc);
//    }
//
//    return ret;
//}


int db_reset_signal_timer(pid_t pid)
{
    int ret = 0;

    struct timespec ts;
    qgis_timer_start(&ts);

    // we need to copy values to a "guaranteed" 64 bit value
    // because the vararg parser assumes type "long long int" with "%l"
    long long sec = ts.tv_sec;
    long long nsec = ts.tv_nsec;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_UPDATE_PROCESS_SIGNAL_TIMER, sec, nsec, (int)pid);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


int db_get_signal_timer(struct timespec *ts, pid_t pid)
{
    assert(ts);
    assert(0 < pid);

    int get_signal_timer(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	struct timespec *ts = data;

	assert(2 == ncol);
	assert(SQLITE_INTEGER == type[0]);
	assert(SQLITE_INTEGER == type[1]);

	ts->tv_sec = results[0].integer;
	ts->tv_nsec = results[1].integer;
	debug(1, "in callback got timer value %ld,%03lds", ts->tv_sec, (ts->tv_nsec/(1000*1000)));

	return 0;
    }

    struct timespec timesp = {0,0};

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_SELECT_PROCESS_SIGNAL_TIMER, get_signal_timer, &timesp, pid);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    *ts = timesp;

    int ret = 0;

    debug(1, "pid %d, value %ld,%03lds. returned %d", pid, ts->tv_sec, (ts->tv_nsec/(1000*1000)), ret);

    return ret;
}


void db_shutdown_get_min_signaltimer(struct timespec *maxtimeval)
{

    int get_signal_timer(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	struct timespec *ts = data;

	assert(2 == ncol);
	assert(SQLITE_INTEGER == type[0]);
	assert(SQLITE_INTEGER == type[1]);

	ts->tv_sec = results[0].integer;
	ts->tv_nsec = results[1].integer;
	    debug(1, "returned value %ld,%03lds", ts->tv_sec, (ts->tv_nsec/(1000*1000)));

	return 0;
    }

    struct timespec timesp = {0,0};

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_SELECT_PROCESS_MIN_SIGNAL_TIMER, get_signal_timer, &timesp);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    *maxtimeval = timesp;

    debug(1, "returned value %ld,%03lds", maxtimeval->tv_sec, (maxtimeval->tv_nsec/(1000*1000)));
}


int db_get_num_shutdown_processes(void)
{

    int get_num_shutdown_processes(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	int *num = data;

	assert(1 == ncol);
	assert(SQLITE_INTEGER == type[0]);

	*num = results[0].integer;

	return 0;
    }

    int num_list = 0;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_GET_NUM_PROCESS_FROM_LIST, get_num_shutdown_processes, &num_list, LIST_SHUTDOWN);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    debug(1, "returned %d", num_list);

    return num_list;
}


int db_remove_process_with_state_exit(void)
{
    int ret = 0;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_DELETE_PROCESS_WITH_STATE, PROC_STATE_EXIT);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


void db_inc_startup_failures(const char *projname)
{
    assert(projname);

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_INC_PROJECT_STARTUP_FAILURE, projname);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }
}


int db_get_startup_failures(const char *projname)
{
    assert(projname);

    int get_startup_failures(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	int *val = data;

	assert(1 == ncol);
	assert(SQLITE_INTEGER == type[0]);

	*val = results[0].integer;
	debug(1, "returned value %d", *val);

	return 0;
    }

    int ret = -1;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_SELECT_PROJECT_STARTUP_FAILURE, get_startup_failures, &ret, projname);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    debug(1, "returned %d", ret);

    return ret;
}


void db_reset_startup_failures(const char *projname)
{
    assert(projname);

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter(DB_RESET_PROJECT_STARTUP_FAILURE, projname);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }
}


char *db_get_project_for_watchid(int watchid)
{
    assert(watchid >= 0);

    int get_project_for_watchid(void *data, int ncol, int *type, union callback_result_t *results, const char**cols)
    {
	const char **val = data;

	assert(1 == ncol);
	assert(SQLITE_TEXT == type[0]);

	*val = strdup((const char *)results[0].text);
	debug(1, "returned value %s", *val);

	return 0;
    }

    char *ret = NULL;

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    db_select_parameter_callback(DB_GET_PROJECT_FROM_WATCHID, get_project_for_watchid, &ret, watchid);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    debug(1, "returned %s", ret);

    return ret;
}


void db_dump(void)
{
    struct callback_data_s {
	int has_printed_headline;
	int bufferlen;
	char *buffer;
    };

    static const int buffer_increase = 8192;

    int dump_tabledata(void *data, int ncol, char **results, char **cols)
    {
	struct callback_data_s *val = data;
	int i;
	if ( !val->has_printed_headline )
	{
	    for (i=0; i<ncol; i++)
	    {
		strcat(val->buffer, cols[i]);
		strcat(val->buffer, ",\t");
	    }
	    val->has_printed_headline = 1;
	}
	strcat(val->buffer, "\n");

	for (i=0; i<ncol; i++)
	{
	    if (results[i])
		strcat(val->buffer, results[i]);
	    else
		strcat(val->buffer, "NULL");
	    strcat(val->buffer, ",\t");
	}
	strcat(val->buffer, "\n");


	return 0;
    }

    struct callback_data_s data = {0};

    data.bufferlen = buffer_increase;
    data.buffer = malloc(data.bufferlen);
    *data.buffer = '\0';	// empty string

    int retval = pthread_mutex_lock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    char *err;
    const char *sql = db_select_statement[DB_DUMP_PROJECT];
    strcat(data.buffer, "PROJECTS:\n");
    sqlite3_exec(dbhandler, sql, dump_tabledata, &data, &err );
    printlog("%s", data.buffer);

    data.has_printed_headline = 0;
    *data.buffer = '\0';	// empty string
    sql = db_select_statement[DB_DUMP_PROCESS];
    strcat(data.buffer, "PROCESSES:\n");
    sqlite3_exec(dbhandler, sql, dump_tabledata, &data, &err );
    printlog("%s", data.buffer);

    retval = pthread_mutex_unlock(&db_lock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    free(data.buffer);
}

