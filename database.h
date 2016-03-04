/*
 * database.h
 *
 *  Created on: 03.03.2016
 *      Author: jh
 */

#ifndef DATABASE_H_
#define DATABASE_H_

#include <sys/types.h>

void db_init(void);
void db_delete(void);


void db_create_project(const char *projname);
void db_create_process(const char *projname, pid_t pid);
int db_get_num_idle_process(const char *projname);
void db_process_killed(pid_t pid);


/* transitional interfaces. these are deleted after the api change */
struct qgis_project_list_s;

struct qgis_project_list_s *db_get_project_list(void);



#endif /* DATABASE_H_ */
