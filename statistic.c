/*
 * statistic.c
 *
 *  Created on: 24.02.2016
 *      Author: jh
 */


#include "statistic.h"

#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>

#include "timer.h"
#include "logger.h"


static struct timespec uptime = {0,0};
static struct timespec connectiontime = {0,0};
static long long int connections = 0;
//static long long int process_crashed = 0;
static long long int process_shutdown = 0;
static long long int process_started = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


void statistic_init(void)
{
    // make shure that this function is called only once
    assert(!(uptime.tv_nsec||uptime.tv_sec));

    qgis_timer_start(&uptime);
}


void statistic_add_connection(const struct timespec *timeradd)
{
    int retval = pthread_mutex_lock(&mutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: lock mutex");
	exit(EXIT_FAILURE);
    }

    qgis_timer_add(&connectiontime, timeradd);
    connections++;

    retval = pthread_mutex_unlock(&mutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex");
	exit(EXIT_FAILURE);
    }

}


//void add_process_crash(int num)
//{
//    int retval = pthread_mutex_lock(&mutex);
//    if (retval)
//    {
//	errno = retval;
//	logerror("ERROR: lock mutex");
//	exit(EXIT_FAILURE);
//    }
//
//    process_crashed += num;
//
//    retval = pthread_mutex_unlock(&mutex);
//    if (retval)
//    {
//	errno = retval;
//	logerror("ERROR: unlock mutex");
//	exit(EXIT_FAILURE);
//    }
//}


void statistic_add_process_shutdown(int num)
{
    int retval = pthread_mutex_lock(&mutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: lock mutex");
	exit(EXIT_FAILURE);
    }

    process_shutdown += num;

    retval = pthread_mutex_unlock(&mutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex");
	exit(EXIT_FAILURE);
    }
}


void statistic_add_process_start(int num)
{
    int retval = pthread_mutex_lock(&mutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: lock mutex");
	exit(EXIT_FAILURE);
    }

    process_started += num;

    retval = pthread_mutex_unlock(&mutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex");
	exit(EXIT_FAILURE);
    }
}


void statistic_printlog(void)
{
    int retval = pthread_mutex_lock(&mutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: lock mutex");
	exit(EXIT_FAILURE);
    }

    struct timespec myconntime = connectiontime;
    long long int myconnections = connections;

    retval = pthread_mutex_unlock(&mutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex");
	exit(EXIT_FAILURE);
    }

    struct timespec myuptime;
    qgis_timer_sub(&uptime, &myuptime);
    /* note: the following types have been taken from time.h
     * it may happen on your system that libc defines this different from glibc
     */
    __time_t mytime = myuptime.tv_sec;
    __time_t seconds = mytime % 60;
    mytime /= 60;
    __time_t minutes = mytime % 60;
    mytime /= 60;
    __time_t hours = mytime % 24;
    mytime /= 24;
    __time_t days = mytime;
    __syscall_slong_t milliseconds = myuptime.tv_nsec / (1000*1000);

    if (0 < myconnections)
    {
	__time_t avg_conn_sec = myconntime.tv_sec/myconnections;
	__time_t avg_conn_msec = ((myconntime.tv_sec%myconnections)*1000) /myconnections;
	avg_conn_msec += myconntime.tv_nsec/((1000*1000)*myconnections);
	if (1000 <= avg_conn_msec)
	{
	    avg_conn_sec += avg_conn_msec/1000;
	    avg_conn_msec %= 1000;
	}

	printlog("Statistics:\n"
		"uptime: %ld days, %02ld:%02ld:%02ld.%03ld hours\n"
		"process started: %lld\n"
		"process shutdown: %lld\n"
//		"process crashed: %lld\n"
		"connections: %lld\n"
		"avg. connection time: %ld.%03ld seconds",
		days, hours, minutes, seconds, milliseconds,
		process_started,
		process_shutdown,
//		process_crashed,
		myconnections,
		avg_conn_sec, avg_conn_msec
	);
    }
    else
    {
	printlog("Statistics:\n"
		"uptime: %ld days, %02ld:%02ld:%02ld.%03ld hours\n"
		"process started: %lld\n"
		"process shutdown: %lld\n"
//		"process crashed: %lld\n"
		"connections: %lld",
		days, hours, minutes, seconds, milliseconds,
		process_started,
		process_shutdown,
//		process_crashed,
		myconnections
	);

    }

}
