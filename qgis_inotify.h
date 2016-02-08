/*
 * qgis_inotify.h
 *
 *  Created on: 05.02.2016
 *      Author: jh
 */

#ifndef QGIS_INOTIFY_H_
#define QGIS_INOTIFY_H_

struct qgis_project_list_s;

void qgis_inotify_init(struct qgis_project_list_s *projectlist);
void qgis_inotify_delete(void);
int qgis_inotify_watch_file(const char *path);


#endif /* QGIS_INOTIFY_H_ */
