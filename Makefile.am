
sbin_PROGRAMS=qgis-schedulerd

qgis_schedulerd_SOURCES=qgis-schedulerd.c \
	qgis_process.c qgis_process_list.c fcgi_state.c fcgi_data.c qgis_config.c qgis_project.c qgis_project_list.c logger.c timer.c qgis_inotify.c qgis_shutdown_queue.c statistic.c database.c process_manager.c \
	qgis_process.h qgis_process_list.h fcgi_state.h fcgi_data.h qgis_config.h qgis_project.h qgis_project_list.h logger.h timer.h qgis_inotify.h qgis_shutdown_queue.h statistic.h database.h process_manager.h

sysconf_DATA = qgis-scheduler.conf
EXTRA_DIST = qgis-scheduler.conf init/README init/gentoo/qgis-scheduler.init init/ubuntu/qgis-scheduler.init
