/*
 * logger.h
 *
 *  Created on: 02.02.2016
 *      Author: jh
 */

/*
    Logging mechanics.
    Provides a logging API to push leveled messages.
    Redirects stdout and stderr to log file.

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


#ifndef LOGGER_H_
#define LOGGER_H_

#include "common.h"


int logger_init(void);
int logger_open_logfile(void);
int printlog(const char *format, ...) __attribute__ ((__format__ (__printf__, 1, 2)));
#define debug(level, format, ...)	mydebug(level, "[%#lx] %s():%d " format, pthread_self(), __FUNCTION__, __LINE__, ## __VA_ARGS__)
int mydebug(int level, const char *format, ...) __attribute__ ((__format__ (__printf__, 2, 3)));
int logerror(const char *format, ...) __attribute__ ((__format__ (__printf__, 1, 2)));


#endif /* LOGGER_H_ */
