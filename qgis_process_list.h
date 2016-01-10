/*
 * qgis_process_list.h
 *
 *  Created on: 08.01.2016
 *      Author: jh
 */

/*
    List manager for the QGIS processes.
    Provides helper functions to search in the list.

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


#ifndef QGIS_PROCESS_LIST_H_
#define QGIS_PROCESS_LIST_H_

#include "qgis_process.h"


struct qgis_process_list_s;
struct qgis_process_iterator;

struct qgis_process_list_s *qgis_process_list_new(void);
void qgis_process_list_delete(struct qgis_process_list_s *list);


void qgis_process_list_add_process(struct qgis_process_list_s *list, struct qgis_process_s *proc);
void qgis_process_list_remove_process(struct qgis_process_list_s *list, struct qgis_process_s *proc);

struct qgis_process_s *qgis_process_list_find_process_by_status(struct qgis_process_list_s *list, enum qgis_process_state_e state);
struct qgis_process_s *qgis_process_list_mutex_find_process_by_status(struct qgis_process_list_s *list, enum qgis_process_state_e state);
struct qgis_process_s *qgis_process_list_find_process_by_pid(struct qgis_process_list_s *list, pid_t pid);
struct qgis_process_s *qgis_process_list_get_first_process(struct qgis_process_list_s *list);
struct qgis_process_iterator *qgis_process_list_get_iterator(struct qgis_process_list_s *list);
struct qgis_process_s *qgis_process_list_get_next_process(struct qgis_process_iterator **iterator);
void qgis_process_list_return_iterator(struct qgis_process_list_s *list);
int qgis_process_list_get_pid_list(struct qgis_process_list_s *list, pid_t **pid, int *len);
int qgis_process_list_get_num_process_by_status(struct qgis_process_list_s *list, enum qgis_process_state_e state);


#endif /* QGIS_PROCESS_LIST_H_ */

