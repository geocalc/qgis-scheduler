/*
 * qgis_project.h
 *
 *  Created on: 10.01.2016
 *      Author: jh
 */

/*
    Database for the QGIS projects.
    Stores the project name and the list of processes working on that project.
    Also stores the path of the qgis configuration file for the processes and
    its inotify watch descriptor. And the compiled regular expression
    to recognize the project by its query string.
    Starts a separate thread to check its configuration file and then reload or
    restart the processes.

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


#ifndef QGIS_PROJECT_H_
#define QGIS_PROJECT_H_

#include "qgis_process_list.h"
#include <regex.h>

struct qgis_project_s;


struct qgis_project_s *qgis_project_new(const char *name, const char *configpath);
void qgis_project_delete(struct qgis_project_s *proj);


regex_t *qgis_project_get_regex(struct qgis_project_s *proj);
struct qgis_process_list_s *qgis_project_get_init_process_list(struct qgis_project_s *proj);
struct qgis_process_list_s *qgis_project_get_active_process_list(struct qgis_project_s *proj);
const char *qgis_project_get_name(struct qgis_project_s *proj);
int qgis_project_add_process(struct qgis_project_s *proj, struct qgis_process_s *proc);
//int qgis_project_shutdown_process(struct qgis_project_s *proj, struct qgis_process_s *proc);
void qgis_project_shutdown(struct qgis_project_s *proj);
int qgis_project_shutdown_all_processes(struct qgis_project_s *proj, int signum);

int qgis_project_process_died(struct qgis_project_s *proj, pid_t pid);
void qgis_project_start_new_process_detached(int num, struct qgis_project_s *project, int do_exchange_processes);
void qgis_project_start_new_process_wait(int num, struct qgis_project_s *project, int do_exchange_processes);
int qgis_project_check_inotify_config_changed(struct qgis_project_s *project, int wd);

void qgis_project_print(struct qgis_project_s *proj);

void qgis_project_inc_nr_crashes(struct qgis_project_s *proj);
int qgis_project_get_nr_crashes(struct qgis_project_s *proj);

#endif /* QGIS_PROJECT_H_ */
