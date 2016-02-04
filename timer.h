/*
 * timer.h
 *
 *  Created on: 04.02.2016
 *      Author: jh
 */

/*
    Timing module.
    This module provides timing facilities, to measure runtime intervalls.

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


#ifndef TIMER_H_
#define TIMER_H_

#include <time.h>


int qgis_timer_start(struct timespec *timer);
int qgis_timer_stop(struct timespec *timer);



#endif /* TIMER_H_ */
