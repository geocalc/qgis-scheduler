/*
 * database.c
 *
 *  Created on: 03.03.2016
 *      Author: jh
 */


#include "database.h"

#include <stdlib.h>
#include <sqlite3.h>
#include <pthread.h>

#include "logger.h"


static sqlite3 *dbhandler = NULL;

/* transitional data. this will be deleted after the code change */
#include "qgis_project_list.h"

struct qgis_project_list_s *projectlist = NULL;





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

struct qgis_project_list_s *db_get_project_list(void)
{
    return projectlist;
}
