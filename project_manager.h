/*
 * project_manager.h
 *
 *  Created on: 04.03.2016
 *      Author: jh
 */

/*
    Management module for the projects.
    Organize startup and shutdown of a whole project.

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


#ifndef PROJECT_MANAGER_H_
#define PROJECT_MANAGER_H_



void project_manager_startup_projects(void);

void project_manager_start_new_process_detached(int num, const char *projectname, int do_exchange_processes);
void project_manager_projectname_configfile_changed(const char *projname);
void project_manager_project_configfile_changed(int inotifyid);

void project_manager_manage_project_changes(const char **newproj, const char **changedproj, const char **deletedproj);
void project_manager_start_project(const char *projname);
void project_manager_restart_project(const char *proj);
void project_manager_shutdown_project(const char *project_name);
void project_manager_shutdown(void);

#endif /* PROJECT_MANAGER_H_ */
