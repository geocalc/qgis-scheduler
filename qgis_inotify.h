/*
 * qgis_inotify.h
 *
 *  Created on: 05.02.2016
 *      Author: jh
 */

/*
    File change tracker.
    Provides a treat to sense changes in the configuration files which belong
    to the child processes.
    Calls a whole project list to check the changed inotify descriptor.

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


#ifndef QGIS_INOTIFY_H_
#define QGIS_INOTIFY_H_


void qgis_inotify_init(void);
void qgis_inotify_delete(void);
int qgis_inotify_watch_file(const char *path);


#endif /* QGIS_INOTIFY_H_ */
