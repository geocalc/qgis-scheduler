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

void db_init(void);
void db_delete(void);


void db_create_project(const char *projname);
void db_create_process(const char *projname, pid_t pid);
int db_get_num_idle_process(const char *projname);
void db_process_died(pid_t pid);


/* transitional interfaces. these are deleted after the api change */
struct qgis_project_list_s;

struct qgis_project_list_s *db_get_project_list(void);



#endif /* DATABASE_H_ */
