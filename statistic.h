/*
 * statistic.h
 *
 *  Created on: 24.02.2016
 *      Author: jh
 */

#ifndef STATISTIC_H_
#define STATISTIC_H_

struct timespec;

void statistic_init(void);

void statistic_add_connection(const struct timespec *timeradd);
void statistic_add_process_crash(int num);
void statistic_add_process_shutdown(int num);
void statistic_add_process_start(int num);

void statistic_printlog(void);


#endif /* STATISTIC_H_ */
