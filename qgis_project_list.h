/*
 * qgis_project_list.h
 *
 *  Created on: 10.01.2016
 *      Author: jh
 */

/*
    List manager for the QGIS projects.
    Each project has a unique configuration file and several processes working
    with that file.
    Provides a list structure to hold information about the projects and the
    attached processes.

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


#ifndef QGIS_PROJECT_LIST_H_
#define QGIS_PROJECT_LIST_H_

#include "qgis_project.h"


struct qgis_project_list_s;
struct qgis_project_iterator;

struct qgis_project_list_s *qgis_proj_list_new(void);
void qgis_proj_list_delete(struct qgis_project_list_s *list);

void qgis_proj_list_add_project(struct qgis_project_list_s *list, struct qgis_project_s *proj);
void qgis_proj_list_remove_project(struct qgis_project_list_s *list, struct qgis_project_s *proj);

struct qgis_project_s *find_project_by_name(struct qgis_project_list_s *list, const char *name);
struct qgis_project_s *qgis_proj_list_find_project_by_pid(struct qgis_project_list_s *list, pid_t pid);

struct qgis_project_iterator *qgis_proj_list_get_iterator(struct qgis_project_list_s *list);
struct qgis_project_s *qgis_proj_list_get_next_project(struct qgis_project_iterator **iterator);
void qgis_proj_list_return_iterator(struct qgis_project_list_s *list);


#endif /* QGIS_PROJECT_LIST_H_ */
