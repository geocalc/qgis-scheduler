/*
 * qgis_config.h
 *
 *  Created on: 15.01.2016
 *      Author: jh
 */

/*
    Database for the scheduler configuration.

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


#ifndef QGIS_CONFIG_H_
#define QGIS_CONFIG_H_


int config_load(const char *path);
void config_shutdown(void);
int config_get_num_projects(void);
const char *config_get_name_project(int num);
const char *config_get_network_listen(void);
const char *config_get_network_port(void);
const char *config_get_user(void);
const char *config_get_pid_path(void);


const char *config_get_process(const char *project);
const char *config_get_process_args(const char *project);
int config_get_min_idle_processes(const char *project);
int config_get_max_idle_processes(const char *project);
const char *config_get_scan_parameter_key(const char *project);
const char *config_get_scan_parameter_regex(const char *project);
const char *config_get_working_directory(const char *project);
const char *config_get_project_config_path(const char *project);


#endif /* QGIS_CONFIG_H_ */
