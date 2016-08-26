/*
 * qgis_shutdown_queue.h
 *
 *  Created on: 10.02.2016
 *      Author: jh
 */

/*
    Separate queue to shutdown child processes.
    It gets the process descriptions and sends them a kill signal and
    waits until the process ended and no more threads are working with them.

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


#ifndef QGIS_SHUTDOWN_QUEUE_H_
#define QGIS_SHUTDOWN_QUEUE_H_

#include <sys/types.h>

#include "common.h"

/* NOTE: This is a workaround. Normally this definition belongs to the main
 *       module and not this shutdown module. But the main module has no
 *       header file, so we move the definition - visible by both modules -
 *       over here.
 */
struct signal_data_s
{
    int signal;
    pid_t pid;
    int is_shutdown;
};


void qgis_shutdown_init(int main_pipe_wr);
void qgis_shutdown_delete(void);
void qgis_shutdown_add_process(pid_t pid);
void qgis_shutdown_add_all_process(const char *project_name);
void qgis_shutdown_notify_changes(void);
void qgis_shutdown_wait_empty(void);
void qgis_shutdown_process_died(pid_t pid);

/* exit handler */
void qexit(int status) __attribute__ ((noreturn));

#endif /* QGIS_SHUTDOWN_QUEUE_H_ */
