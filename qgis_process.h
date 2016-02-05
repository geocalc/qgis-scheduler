/*
 * qgis_process.h
 *
 *  Created on: 06.01.2016
 *      Author: jh
 */

/*
    Database for the QGIS processes.
    Stores file descriptors, status and thread information for the child processes.
    This module does not act on the information, it just stores it to provide for
    future use.

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


#ifndef QGIS_PROCESS_H_
#define QGIS_PROCESS_H_

#include <unistd.h>
#include <pthread.h>


struct qgis_process_s;
struct timespec;

enum qgis_process_state_e
{
    PROC_START = 0,
    PROC_INIT,
    PROC_IDLE,
    PROC_OPEN_IDLE,
    PROC_BUSY,
};

struct qgis_process_s *qgis_process_new(pid_t pid, int process_socket_fd);
void qgis_process_delete(struct qgis_process_s *proc);

int qgis_process_set_state_idle(struct qgis_process_s *proc);
int qgis_process_set_state_busy(struct qgis_process_s *proc, pthread_t thread_id);
//int qgis_process_set_state_idle_open(struct qgis_process_s *proc);
int qgis_process_set_state_init(struct qgis_process_s *proc, pthread_t thread_id);
enum qgis_process_state_e qgis_process_get_state(struct qgis_process_s *proc);

int qgis_process_get_socketfd(struct qgis_process_s *proc);
pthread_mutex_t *qgis_process_get_mutex(struct qgis_process_s *proc);
pid_t qgis_process_get_pid(struct qgis_process_s *proc);
const struct timespec *qgis_process_get_starttime(struct qgis_process_s *proc);


#endif /* QGIS_PROCESS_H_ */
