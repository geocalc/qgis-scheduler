
sbin_PROGRAMS=qgis-schedulerd

qgis_schedulerd_SOURCES=qgis-schedulerd.c common.h \
	fcgi_state.c fcgi_data.c qgis_config.c logger.c timer.c qgis_inotify.c qgis_shutdown_queue.c statistic.c database.c process_manager.c connection_manager.c project_manager.c stringext.c \
	fcgi_state.h fcgi_data.h qgis_config.h logger.h timer.h qgis_inotify.h qgis_shutdown_queue.h statistic.h database.h process_manager.h connection_manager.h project_manager.h stringext.h

sysconf_DATA = qgis-scheduler.conf
EXTRA_DIST = qgis-scheduler.conf init/README init/gentoo/qgis-scheduler.init init/ubuntu/qgis-schedulerd.init

dist_man1_MANS = qgis-schedulerd.man
dist_man5_MANS = qgis-scheduler.conf.man
