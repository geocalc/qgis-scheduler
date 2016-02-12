/*
 * timer.c
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


#include "timer.h"

#include <assert.h>

#include "qgis_config.h"


int qgis_timer_start(struct timespec *timer)
{
    assert(timer);
    int retval = clock_gettime(get_valid_clock_id(), timer);

    return retval;
}


int qgis_timer_stop(struct timespec *timer)
{
    assert(timer);

    struct timespec oldtime = *timer;
    struct timespec newtime;

    int retval = clock_gettime(get_valid_clock_id(), &newtime);
    if (-1 != retval)
    {
	    timer->tv_sec = newtime.tv_sec - oldtime.tv_sec;
	    timer->tv_nsec = newtime.tv_nsec - oldtime.tv_nsec;
	    if (0 > timer->tv_nsec)
	    {
		timer->tv_nsec += 1000*1000*1000;
		timer->tv_sec--;
	    }
    }

    return retval;
}


/* calculates the difference between the time specified in "timer" and the
 * current time. The result is stored in "timersub".
 * Returns -1 in case of an error.
 */
int qgis_timer_sub(const struct timespec *timer, struct timespec *timersub)
{
    assert(timer);
    assert(timersub);

    struct timespec newtime;

    int retval = clock_gettime(get_valid_clock_id(), &newtime);
    if (-1 != retval)
    {
	    timersub->tv_sec = newtime.tv_sec - timer->tv_sec;
	    timersub->tv_nsec = newtime.tv_nsec - timer->tv_nsec;
	    if (0 > timersub->tv_nsec)
	    {
		timersub->tv_nsec += 1000*1000*1000;
		timersub->tv_sec--;
	    }
    }

    return retval;
}


/* simply adds "timeradd" to "timer"
 */
void qgis_timer_add(struct timespec *timer, const struct timespec *timeradd)
{
    assert(timer);
    assert(timeradd);

    timer->tv_sec += timeradd->tv_sec;
    timer->tv_nsec += timeradd->tv_nsec;
}


int qgis_timer_isgreaterthan(const struct timespec *timer1, const struct timespec *timer2)
{
    assert(timer1);
    assert(timer2);

    if ( (timer1->tv_sec > timer2->tv_sec)
	 ||
	 (
		 (timer1->tv_sec == timer2->tv_sec)
		 &&
		 (timer1->tv_nsec > timer2->tv_nsec)
	 )
       )
	return 1;

    return 0;
}


int qgis_timer_is_empty(const struct timespec *timer)
{
    assert(timer);

    if (timer->tv_sec || timer->tv_nsec)
	return 0;

    return 1;
}



