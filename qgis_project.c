/*
 * qgis_project.c
 *
 *  Created on: 10.01.2016
 *      Author: jh
 */

/*
    Database for the QGIS projects.
    Stores the project name and the list of processes working on that project.
    Also stores the path of the qgis configuration file for the processes and
    its inotify watch descriptor.

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


#include "qgis_project.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fastcgi.h>
#include <sys/inotify.h>
#include <libgen.h>	// used for dirname(), we need glibc >= 2.2.1 !!
#include <limits.h>

#include "qgis_process_list.h"
#include "qgis_config.h"
#include "fcgi_state.h"


//#define DISABLED_INIT



struct qgis_project_s
{
    struct qgis_process_list_s *initproclist;	// list of processes which are initialized and can not be used for requests
    struct qgis_process_list_s *activeproclist;	// list of processes which handle fcgi requests
    struct qgis_process_list_s *shutdownproclist; // list of processes which are shut down
    const char *name;
    const char *configpath;		// full path to qgis config file watched for updates. note: no watch if configpath is NULL
    const char *configbasename;		// only config file name without path
    pthread_t config_watch_threadid;
    pthread_rwlock_t rwlock;
    int inotifyfd;
    int inotifywatchfd;
};


struct thread_watch_config_args
{
    struct qgis_project_s *project;
};


struct thread_init_new_child_args
{
    struct qgis_process_s *proc;
    const char *project_name;
};


struct thread_start_new_child_args
{
    struct qgis_project_s *project;
};


/* constants */
static const char base_socket_desc[] = "qgis-schedulerd-socket";



/* This global number is counted upwards, overflow included.
 * Every new unix socket gets this number attached creating a unique socket
 * path. In the very unlikely case the number is already given to an existing
 * socket path, the number is counted upwards again until we find a path which
 * is not assigned to a socket.
 * I assume here we do not create UINT_MAX number of sockets in this computer..
 */
static unsigned int socket_id = 0;
static pthread_mutex_t socket_id_mutex = PTHREAD_MUTEX_INITIALIZER;


void start_new_process_wait(int num, struct qgis_project_s *project, int do_exchange_processes);


/* restarts all processes.
 * I.e. evaluate current number of processes for this project,
 * start num processes, init them,
 * atomically move the old processes to shutdown list
 * and new processes to active list,
 * and kill all old processes from shutdown list.
 */
void qgis_project_restart_processes(struct qgis_project_s *project)
{
    assert(project);
    if (project)
    {
	int numproc = qgis_process_list_get_num_process(project->activeproclist);
	start_new_process_wait(numproc, project, 1);
    }
}


void qgis_project_config_change(struct qgis_project_s *project, const char *filename)
{
    /* this file has been changed.
     * check if the file belongs to our projects
     */
    assert(project);
    assert(filename);

    int retval = strcmp(filename, project->configbasename);
    if (0 == retval)
    {
	/* match, start new processes and then move them to idle list */
	fprintf(stderr, "found config change in project '%s', update processes\n", project->name);
	qgis_project_restart_processes(project);
    }

}



/* watches changes in the configuration file.
 * uses inotify.
 */
static void *thread_watch_config(void *arg)
{
    assert(arg);
    struct thread_watch_config_args *tinfo = arg;
    struct qgis_project_s *project = tinfo->project;
    assert(project);
    pthread_t thread_id = pthread_self();


    const char *projname = project->name;
    fprintf(stderr, "[%lu] started watcher thread for project '%s', looking for changes in '%s'\n", thread_id, projname, project->configpath);


    static const int sizeof_inotifyevent = sizeof(struct inotify_event) + NAME_MAX + 1;
    struct inotify_event *inotifyevent = malloc(sizeof_inotifyevent);
    assert(inotifyevent);
    if ( !inotifyevent )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    for (;;)
    {
	int retval = read(project->inotifyfd, inotifyevent, sizeof_inotifyevent);
	if (-1 == retval)
	{
	    switch (errno)
	    {
	    case EINTR:
		/* We received an interrupt, possibly a termination signal.
		 */
		fprintf(stderr, "[%lu] read() inotify_event received interrupt\n", thread_id);
		break;

	    default:
		perror("read() inotify_event");
		exit(EXIT_FAILURE);
		// no break needed
	    }
	}
	else
	{
	    int size_read = retval;
	    fprintf(stderr, "[%lu] inotify read %d bytes, sizeof event %lu, len %u\n", thread_id, size_read, sizeof(*inotifyevent), inotifyevent->len);

	    while (size_read >= sizeof(*inotifyevent) + inotifyevent->len)
	    {
		if (inotifyevent->wd == project->inotifywatchfd)
		{
		    switch(inotifyevent->mask)
		    {
		    case IN_CLOSE_WRITE:
			/* The file has been written or copied to this path
			 * Restart the processes
			 */
			fprintf(stderr, "got event IN_CLOSE_WRITE for project %s\n", projname );
			fprintf(stderr, "mask 0x%x, len %d, name %s\n", inotifyevent->mask, inotifyevent->len, inotifyevent->name);
			qgis_project_config_change(project, inotifyevent->name);
			break;

		    case IN_DELETE:
			/* The file has been erased from this path.
			 * Don't care, just mark the service as not restartable. (or better close this project and kill child progs?)
			 */
			fprintf(stderr, "got event IN_DELETE for project %s\n", projname );
			fprintf(stderr, "mask 0x%x, len %d, name %s\n", inotifyevent->mask, inotifyevent->len, inotifyevent->name);
			break;

		    case IN_MOVED_TO:
			/* The file has been overwritten by a move to this path
			 * Restart the processes
			 */
			fprintf(stderr, "got event IN_MOVED_TO for project %s\n", projname );
			fprintf(stderr, "mask 0x%x, len %d, name %s\n", inotifyevent->mask, inotifyevent->len, inotifyevent->name);
			qgis_project_config_change(project, inotifyevent->name);
			break;

		    case IN_IGNORED:
			// Watch was removed. We can exit this thread
			fprintf(stderr, "got event IN_IGNORED for project %s\n", projname );
			//		fprintf(stderr, "mask 0x%x, len %d, name %s\n", inotifyevent->mask, inotifyevent->len, inotifyevent->name);
			if (get_program_shutdown())
			{
			    goto thread_watch_config_end_for_loop;
			}
			break;

		    default:
			fprintf(stderr, "error: got unexpected event %d for project %s\n", inotifyevent->mask, projname );
			break;
		    }
		}
		else
		{
		    fprintf(stderr, "error: project %s, got event %d for unknown watch %d. Ignored\n", projname, inotifyevent->mask, inotifyevent->wd );
		}

		size_read -= sizeof(*inotifyevent) + inotifyevent->len;
	    }
	    assert(0 == size_read);

	}
    }

thread_watch_config_end_for_loop:

    fprintf(stderr, "[%lu] shutdown watcher thread for project '%s'\n", thread_id, projname);
    free(inotifyevent);
    free(arg);
    return NULL;
}


struct qgis_project_s *qgis_project_new(const char *name, const char *configpath)
{
    assert(name);
    //assert(configpath); configpath is allowed to be NULL. in this case no watcher thread is started

    struct qgis_project_s *proj = calloc(1, sizeof(*proj));
    assert(proj);
    if ( !proj )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }
    proj->initproclist = qgis_process_list_new();
    proj->activeproclist = qgis_process_list_new();
    proj->shutdownproclist = qgis_process_list_new();
    proj->name = name;

    int retval = pthread_rwlock_init(&proj->rwlock, NULL);
    if (retval)
    {
	errno = retval;
	perror("error init read-write lock");
	exit(EXIT_FAILURE);
    }

    /* if the path to a configuration file has been given and the path
     * is correct (stat()), then watch the file with inotify for changes
     * If the file changed, kill all processes and restart them anew.
     */
    // TODO: create a separate inotify watch module
    if (configpath)
    {
	struct stat statbuf;
	retval = stat(configpath, &statbuf);
	if (-1 == retval)
	{
	    switch(errno)
	    {
	    case EACCES:
	    case ELOOP:
	    case EFAULT:
	    case ENAMETOOLONG:
	    case ENOENT:
	    case ENOTDIR:
	    case EOVERFLOW:
		fprintf(stderr, "error accessing file '%s': ", configpath);
		perror(NULL);
		fprintf(stderr, "file is not watched for changes\n");
		break;

	    default:
		fprintf(stderr, "error accessing file '%s': ", configpath);
		perror(NULL);
		exit(EXIT_FAILURE);
	    }
	}
	else
	{
	    if (S_ISREG(statbuf.st_mode))
	    {
		/* if I can stat the file I assume we can read it.
		 * Now setup the inotify descriptor.
		 *
		 * The process should be restarted if a new configuration is
		 * available.
		 * This may happen by editing the file in place (1), moving another
		 * file to the same place (2), copying another file to the same
		 * place (3) or creating the file in the directory (4).
		 * Looking at the man page we get:
		 * In case of (1) we receive the event IN_CLOSE_WRITE for the file
		 * and the directory as well.
		 * In case of (2) we receive the events IN_ATTRIB, IN_DELETE_SELF
		 * and IN_IGNORED for the configuration file, and IN_DELETE and
		 * IN_MOVED_TO for the directory.
		 * In case of (3) we receive the event IN_CLOSE_WRITE for the
		 * directory.
		 * In case of (4) we receive the event IN_CLOSE_WRITE for the
		 * directory.
		 * If the file is deleted the project may reject starting further
		 * processes and keep the current ones to feed the network clients.
		 *
		 * With some tests we get:
		 * Looking at the directory and the target file name alone we get
		 * the event IN_CLOSE_WRITE for the case (1), (3) and (4) and the
		 * event IN_MOVED_TO for the case (2).
		 *
		 * So in summary we watch the directory for changes and react on
		 * the file name of the configuration file.
		 *
		 * Currently we do not handle deleting and creating of the whole
		 * directory. This case may expose some bugs though..
		 */
		retval = inotify_init1(IN_CLOEXEC);
		if (-1 == retval)
		{
		    perror("inotify_init1");
		    exit(EXIT_FAILURE);
		}
		proj->inotifyfd = retval;

		char *directoryname = strdup(configpath);
		assert(directoryname);
		if ( !directoryname )
		{
		    perror("could not allocate memory");
		    exit(EXIT_FAILURE);
		}

		directoryname = dirname(directoryname);

		retval = inotify_add_watch(proj->inotifyfd, directoryname, IN_CLOSE_WRITE|IN_DELETE|IN_MOVED_TO|IN_IGNORED);
		if (-1 == retval)
		{
		    perror("inotify_add_watch");
		    exit(EXIT_FAILURE);
		}
		proj->inotifywatchfd = retval;

		proj->configbasename = basename((char *)configpath);
		proj->configpath = configpath;

		/* start a new thread to watch the configuration file
		 */
		/* NOTE: aside from the general rule
		 * "malloc() and free() within the same function"
		 * we transfer the responsibility for this memory
		 * to the thread itself.
		 */
		struct thread_watch_config_args *targs = malloc(sizeof(*targs));
		assert(targs);
		if ( !targs )
		{
		    perror("could not allocate memory");
		    exit(EXIT_FAILURE);
		}
		targs->project = proj;

		retval = pthread_create(&proj->config_watch_threadid, NULL, thread_watch_config, targs);
		if (retval)
		{
		    errno = retval;
		    perror("error creating thread");
		    exit(EXIT_FAILURE);
		}

		free(directoryname);
	    }
	    else
	    {
		fprintf(stderr, "error '%s' is no regular file\n", configpath);
	    }
	}
    }

    return proj;
}


void qgis_project_delete(struct qgis_project_s *proj)
{
    if (proj)
    {
	fprintf(stderr, "deleting project '%s'\n", proj->name);
	if (proj->inotifyfd)
	{
	    fprintf(stderr, "found inotify fd for project '%s'\n", proj->name);
	    int retval;
	    if (proj->inotifywatchfd)
	    {
		fprintf(stderr, "removing inotify watch for project '%s'\n", proj->name);
		retval = inotify_rm_watch(proj->inotifyfd, proj->inotifywatchfd);
		if (-1 == retval)
		{
		    perror("inotify_rm_watch");
		    // no exit on purpose
		}

		// if we have a valid inotify watch fd, then there must be a watcher thread?
		assert(proj->config_watch_threadid);
		fprintf(stderr, "[%lu] join process %lu\n", pthread_self(), proj->config_watch_threadid);
		int retval = pthread_join(proj->config_watch_threadid, NULL);
		if (retval)
		{
		    errno = retval;
		    perror("error joining thread");
		    exit(EXIT_FAILURE);
		}
	    }

	    fprintf(stderr, "close inotify fd for project '%s'\n", proj->name);
	    close(proj->inotifyfd);
	}

	qgis_process_list_delete(proj->initproclist);
	qgis_process_list_delete(proj->activeproclist);
	qgis_process_list_delete(proj->shutdownproclist);
	free(proj);
    }
}


int qgis_project_add_process(struct qgis_project_s *proj, struct qgis_process_s *proc)
{
    assert(proj);
    assert(proc);
    assert(proj->initproclist);
    if (proj && proj->initproclist && proc)
    {
	qgis_process_list_add_process(proj->initproclist, proc);

	return 0;
    }

    return -1;
}


static void qgis_project_thread_function_init_new_child(struct thread_init_new_child_args *tinfo)
{
    assert(tinfo);
    struct qgis_process_s *childproc = tinfo->proc;
    assert(childproc);
    const char *projname = tinfo->project_name;
    assert(projname);
    const pthread_t thread_id = pthread_self();
    char *buffer;
    int retval;

    qgis_process_set_state_init(childproc, thread_id);


    fprintf(stderr, "init new spawned child process for project '%s'\n", projname);


//    char debugfile[256];
//    sprintf(debugfile, "/tmp/threadinit.%lu.dump", thread_id);
//    int debugfd = open(debugfile, (O_WRONLY|O_CREAT|O_TRUNC), (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH));
//    if (-1 == debugfd)
//    {
//	fprintf(stderr, "error can not open file '%s': ", debugfile);
//	perror(NULL);
//	exit(EXIT_FAILURE);
//    }


    /* open the socket to the child process */

    struct sockaddr_un sockaddr;
    socklen_t sockaddrlen = sizeof(sockaddr);
    int childunixsocketfd = qgis_process_get_socketfd(childproc);

    retval = getsockname(childunixsocketfd, (struct sockaddr *)&sockaddr, &sockaddrlen);
    if (-1 == retval)
    {
	perror("error retrieving the name of child process socket");
	exit(EXIT_FAILURE);
    }
    /* leave the original child socket and create a new one on the opposite
     * side.
     */
    retval = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (-1 == retval)
    {
	perror("error: can not create socket to child process");
	exit(EXIT_FAILURE);
    }
    childunixsocketfd = retval;	// refers to the socket this program connects to the child process
    retval = connect(childunixsocketfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));
    if (-1 == retval)
    {
	perror("error: can not connect to child process");
	exit(EXIT_FAILURE);
    }



    /* create the fcgi data and
     * send the fcgi data to the child process
     */
    static const int maxbufferlen = 4096;
    buffer = malloc(maxbufferlen);
    assert(buffer);
    if ( !buffer )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    static const int requestid = 1;
    int len;
    struct fcgi_message_s *message = fcgi_message_new_begin(requestid, FCGI_RESPONDER, 0);
    len = fcgi_message_write(buffer, maxbufferlen, message);
    if (-1 == len)	// TODO: be more flexible if buffer too small
    {
	fprintf(stderr, "fcgi message buffer too small (%d)\n", maxbufferlen);
	exit(EXIT_FAILURE);
    }
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	perror("error: can not write to child process");
	exit(EXIT_FAILURE);
    }
    //printf(stderr, "write to child prog (%d): %.*s\n", retval, buffer, retval);
    fcgi_message_delete(message);

    {
	char *parambuffer = (char *)buffer;
	int remain_len = maxbufferlen;

	int i;
	for (i=0; i<128; i++)
	{
	    const char *key = config_get_init_key(projname, i);
	    if (!key)
		break;
	    const char *value = config_get_init_value(projname, i);
	    if (!value)
		break;
	    fprintf(stderr, "Param %s=%s\n", key, value);

	    retval = fcgi_param_list_write(parambuffer, remain_len, key, value);
	    if (-1 == retval)
	    {
		// TODO: be more flexible if buffer too small
		fprintf(stderr, "fcgi parameter buffer too small (%d)\n", maxbufferlen);
		exit(EXIT_FAILURE);

	    }

	    parambuffer += retval;
	    remain_len -= retval;

	}
	len = maxbufferlen - remain_len;

	if (i>=128)
	{
	    fprintf(stderr, "fcgi parameter too many key/value pairs\n");
	    exit(EXIT_FAILURE);

	}
    }

    /* send parameter list */
    message = fcgi_message_new_parameter(requestid, buffer, len);
    len = fcgi_message_write(buffer, maxbufferlen, message);
    if (-1 == len)	// TODO: be more flexible if buffer too small
    {
	fprintf(stderr, "fcgi message buffer too small (%d)\n", maxbufferlen);
	exit(EXIT_FAILURE);
    }
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	perror("error: can not write to child process");
	exit(EXIT_FAILURE);
    }
    fcgi_message_delete(message);

    /* send empty parameter list to signal EOP */
    message = fcgi_message_new_parameter(requestid, "", 0);
    len = fcgi_message_write(buffer, maxbufferlen, message);
    if (-1 == len)	// TODO: be more flexible if buffer too small
    {
	fprintf(stderr, "fcgi message buffer too small (%d)\n", maxbufferlen);
	exit(EXIT_FAILURE);
    }
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	perror("error: can not write to child process");
	exit(EXIT_FAILURE);
    }
    fcgi_message_delete(message);


    message = fcgi_message_new_stdin(requestid, "", 0);
    len = fcgi_message_write(buffer, maxbufferlen, message);
    if (-1 == len)
    {
	fprintf(stderr, "fcgi message buffer too small (%d)\n", maxbufferlen);
	exit(EXIT_FAILURE);
    }
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	perror("error: can not write to child process");
	exit(EXIT_FAILURE);
    }
    // write stdin = "" twice
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	perror("error: can not write to child process");
	exit(EXIT_FAILURE);
    }
    fcgi_message_delete(message);


    /* now read from socket into void until no more data
     * we do it to make sure that the child process has completed the request
     * and filled up its cache.
     */
    retval = 1;
    while (retval>0)
    {
	retval = read(childunixsocketfd, buffer, maxbufferlen);
    }


    /* ok, we did read each and every byte from child process.
     * now close this and set idle
     */
    close(childunixsocketfd);
//    close(debugfd);
    fprintf(stderr, "init child process for project '%s' done. waiting for input..\n", projname);

    // TODO: do we need this distinction over here?
    if (childproc)
    {
	qgis_process_set_state_idle(childproc);
    }
    else
    {
	fprintf(stderr, "no child process found to initialize\n");
	exit(EXIT_FAILURE);
    }
    free(buffer);
}



static struct qgis_process_s *qgis_project_thread_function_start_new_child(struct thread_start_new_child_args *tinfo)
{
    assert(tinfo);
    struct qgis_project_s *project = tinfo->project;
    const char *project_name = qgis_project_get_name(project);
    const char *command = config_get_process( project_name );

    fprintf(stderr, "start new child process\n");

    if (NULL == command || 0 == strlen(command))
    {
	fprintf(stderr, "error: no process path specified. Not starting any process\n");
	return NULL;
    }

    /* prepare the socket to connect to this child process only */
    /* NOTE: Linux allows abstract socket names which have no representation
     * in the filesystem namespace.
     */
    int retval = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (-1 == retval)
    {
	perror("can not create socket for fcgi program");
	exit(EXIT_FAILURE);
    }
    const int childsocket = retval;


    /* Create a unique socket name without a file system inode.
     * The name is "\0qgis-schedulerd-socket0", "..1", etc.
     * We just count upwards until integer overflow and then start
     * over at 0.
     * If one name is already given to a socket, bind() returns EADDRINUSE.
     * In this case we try again.
     */

    /* security rope for the really unlikely case
     * that we got no more numbers free.
     * To prevent infinite loop
     */
    retval = pthread_mutex_lock (&socket_id_mutex);
    if (retval)
    {
	errno = retval;
	perror("error acquire mutex");
	exit(EXIT_FAILURE);
    }
    unsigned int socket_suffix_start = socket_id-1;
    retval = pthread_mutex_unlock (&socket_id_mutex);
    if (retval)
    {
	errno = retval;
	perror("error unlock mutex");
	exit(EXIT_FAILURE);
    }

    for (;;)
    {
	retval = pthread_mutex_lock (&socket_id_mutex);
	if (retval)
	{
	    errno = retval;
	    perror("error acquire mutex");
	    exit(EXIT_FAILURE);
	}
	unsigned int socket_suffix = socket_id++;
	retval = pthread_mutex_unlock (&socket_id_mutex);
	if (retval)
	{
	    errno = retval;
	    perror("error unlock mutex");
	    exit(EXIT_FAILURE);
	}

	if (socket_suffix == socket_suffix_start)
	{
	    /* we tested UINT_MAX numbers without success.
	     * exit here, because we can not get any more numbers.
	     * Or we have a programmers error here..
	     */
	    fprintf(stderr, "error: out of numbers to create socket name. exit");
	    exit(EXIT_FAILURE);
	}

	struct sockaddr_un childsockaddr;
	childsockaddr.sun_family = AF_UNIX;
	retval = snprintf( childsockaddr.sun_path, sizeof(childsockaddr.sun_path), "%c%s%u", '\0', base_socket_desc, socket_suffix );
	if (-1 == retval)
	{
	    perror("error calling string format function snprintf");
	    exit(EXIT_FAILURE);
	}

	retval = bind(childsocket, (struct sockaddr *)&childsockaddr, sizeof(childsockaddr));
	if (-1 == retval)
	{
	    if (EADDRINUSE==errno)
	    {
		continue;	// reiterate with next number
	    }
	    else
	    {
		perror("error calling bind");
		exit(EXIT_FAILURE);
	    }
	}
	break;
    }

    /* the child process listens for connections, one at a time */
    retval = listen(childsocket, 1);
    if (-1 == retval)
    {
	perror("can not listen to socket connecting fast cgi application");
	exit(EXIT_FAILURE);
    }



    pid_t pid = fork();
    if (0 == pid)
    {
	/* child */

	/* change working dir
	 * close file descriptor stdin = 0
	 * assign socket file descriptor to fd 0
	 * fork
	 * exec
	 */
	int ret = chdir(config_get_working_directory(project_name));
	if (-1 == ret)
	{
	    perror("error calling chdir");
	}


	ret = dup2(childsocket, FCGI_LISTENSOCK_FILENO);
	if (-1 == ret)
	{
	    perror("error calling dup2");
	    exit(EXIT_FAILURE);
	}


	/* close all file descriptors different from 0. The fd different from
	 * 1 and 2 are opened during open() and socket() calls with FD_CLOEXEC
	 * flag enabled. This way, all fds are closed during exec() call.
	 * TODO: assign an error log file to fd 1 and 2
	 */
	close(STDOUT_FILENO);
	close(STDERR_FILENO);


	execl(command, command, NULL);
	fprintf(stderr, "could not execute '%s': ", command);
	perror(NULL);
	exit(EXIT_FAILURE);
    }
    else if (0 < pid)
    {
	/* parent */
	struct qgis_process_s *childproc = qgis_process_new(pid, childsocket);
	qgis_project_add_process(project, childproc);

	return childproc;
    }
    else
    {
	/* error */
	perror("can not fork");
	exit(EXIT_FAILURE);
    }

    return NULL;
}

void *thread_start_new_child(void *arg)
{
    assert(arg);
    struct thread_start_new_child_args *tinfo = arg;
    struct thread_init_new_child_args initargs;

    initargs.proc = qgis_project_thread_function_start_new_child(arg);
#ifdef DISABLED_INIT
#warning disabled init phase
    qgis_process_set_state_idle(initargs.proc);
#else
    if (initargs.proc)
    {
	initargs.project_name = qgis_project_get_name(tinfo->project);
	qgis_project_thread_function_init_new_child(&initargs);
    }
#endif

    free(arg);
    return NULL;
}

/* starts "num" new child processes synchronously.
 * param num: number of processes to start (num>=0)
 * param project: project to manage them
 * param do_exchange_processes: if true removes all active processes and replaces them with the new created ones.
 *                              else integrate them in the list of active processes.
 */
void start_new_process_wait(int num, struct qgis_project_s *project, int do_exchange_processes)
{
    pthread_t threads[num];
    int i;
    int retval;

    /* start all thread in parallel */
    for (i=0; i<num; i++)
    {
	/* NOTE: aside from the general rule
	 * "malloc() and free() within the same function"
	 * we transfer the responsibility for this memory
	 * to the thread itself.
	 */
	struct thread_start_new_child_args *targs = malloc(sizeof(*targs));
	assert(targs);
	if ( !targs )
	{
	    perror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	targs->project = project;

	retval = pthread_create(&threads[i], NULL, thread_start_new_child, targs);
	if (retval)
	{
	    errno = retval;
	    perror("error creating thread");
	    exit(EXIT_FAILURE);
	}
	fprintf(stderr, "[%lu] started process %lu\n", pthread_self(), threads[i]);
    }
    /* wait for those threads */
    for (i=0; i<num; i++)
    {
	fprintf(stderr, "[%lu] join process %lu\n", pthread_self(), threads[i]);
	retval = pthread_join(threads[i], NULL);
	if (retval)
	{
	    errno = retval;
	    perror("error joining thread");
	    exit(EXIT_FAILURE);
	}
    }


    /* move the processes from the initialization list to the active process
     * list
     */
    retval = pthread_rwlock_wrlock(&project->rwlock);
    if (retval)
    {
	errno = retval;
	perror("error acquire read-write lock");
	exit(EXIT_FAILURE);
    }

    if (do_exchange_processes)
    {
	retval = qgis_process_list_transfer_all_process(project->shutdownproclist, project->activeproclist);
	fprintf(stderr, "project '%s' moved %d processes from active list to shutdown list\n", project->name, retval);
	retval = qgis_process_list_send_signal(project->shutdownproclist, SIGTERM);
	fprintf(stderr, "project '%s' send %d processes the TERM signal\n", project->name, retval);
    }
    retval = qgis_process_list_transfer_all_process_with_state(project->activeproclist, project->initproclist, PROC_IDLE);
    fprintf(stderr, "project '%s' moved %d processes from init list to active list\n", project->name, retval);

    retval = pthread_rwlock_unlock(&project->rwlock);
    if (retval)
    {
	errno = retval;
	perror("error unlock read-write lock");
	exit(EXIT_FAILURE);
    }


}


struct thread_start_process_detached_args
{
    int num;
    struct qgis_project_s *project;
    int do_exchange_processes;
};


void *qgis_project_thread_start_process_detached(void *arg)
{
    assert(arg);
    struct thread_start_process_detached_args *tinfo = arg;
    pthread_t thread_id = pthread_self();

    /* detach myself from the main thread. Doing this to collect resources after
     * this thread ends. Because there is no join() waiting for this thread.
     */
    int retval = pthread_detach(thread_id);
    if (retval)
    {
	errno = retval;
	perror("error detaching thread");
	exit(EXIT_FAILURE);
    }

    start_new_process_wait(tinfo->num, tinfo->project, tinfo->do_exchange_processes);

    return NULL;
}


/* starts a new thread in detached state, which in turn calls start_new_process_wait() */
void start_new_process_detached(int num, struct qgis_project_s *project, int do_exchange_processes)
{
    /* Start the process creation thread in detached state because
     * we do not want to wait for it. Different from the handling
     * during the program startup there is no join() waiting for
     * the end of the thread and collecting its resources.
     */

    /* NOTE: aside from the general rule
     * "malloc() and free() within the same function"
     * we transfer the responsibility for this memory
     * to the thread itself.
     */
    struct thread_start_process_detached_args *targs = malloc(sizeof(*targs));
    assert(targs);
    if ( !targs )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }
    targs->num = num;
    targs->project = project;
    targs->do_exchange_processes = do_exchange_processes;

    pthread_t thread;
    int retval = pthread_create(&thread, NULL, qgis_project_thread_start_process_detached, targs);
    if (retval)
    {
	errno = retval;
	perror("error: pthread_create");
	exit(EXIT_FAILURE);
    }

}



/* some process died and sent its pid via SIGCHLD.
 * maybe it was listed in this project?
 * test and remove the old entry and maybe restart anew.
 * test all three lists initproclist, activeproclist and shutdownproclist.
 */
void qgis_project_process_died(struct qgis_project_s *proj, pid_t pid)
{
    assert(proj);
    assert(pid>0);

    if (proj)
    {
	/* Erase the old entry. The process does not exist anymore */
	struct qgis_process_list_s *proclist = proj->activeproclist;
	assert(proclist);
	struct qgis_process_s *proc = qgis_process_list_find_process_by_pid(proclist, pid);
	if (proc)
	{
	    /* that process belongs to our active list.
	     * restart the process if not during shutdown.
	     */
	    qgis_process_list_remove_process(proclist, proc);
	    qgis_process_delete(proc);

	    if ( !get_program_shutdown() )
	    {
		fprintf(stderr, "project '%s' restarting process\n", proj->name);

		/* child process terminated, restart anew */
		/* TODO: react on child processes exiting immediately.
		 * maybe store the creation time and calculate the execution time?
		 */
		start_new_process_detached(1, proj, 0);
	    }
	}
	else
	{
	    proclist = proj->initproclist;
	    assert(proclist);
	    proc = qgis_process_list_find_process_by_pid(proclist, pid);
	    if (proc)
	    {
		/* that process belongs to our active list.
		 * restart the process if not during shutdown.
		 */
		qgis_process_list_remove_process(proclist, proc);
		qgis_process_delete(proc);

		if ( !get_program_shutdown() )
		{
		    fprintf(stderr, "project '%s' restarting process\n", proj->name);

		    /* child process terminated, restart anew */
		    /* TODO: react on child processes exiting immediately.
		     * maybe store the creation time and calculate the execution time?
		     */
		    start_new_process_detached(1, proj, 0);
		}
	    }
	    else

	    {
		proclist = proj->shutdownproclist;
		if (proclist)
		{
		    proc = qgis_process_list_find_process_by_pid(proclist, pid);
		    if (proc)
		    {
			/* that process belongs to our list of deleted processes.
			 * just remove it from this list.
			 */
			qgis_process_list_remove_process(proclist, proc);
			qgis_process_delete(proc);
		    }
		}
	    }
	}
    }
}


//int qgis_project_shutdown_process(struct qgis_project_s *proj, struct qgis_process_s *proc)
//{
//
//    return -1;
//}


/* Get a list of al processes belonging to this project and send them the kill
 * signal "signum".
 * return: number of processes send "signum" */
int qgis_project_shutdown_all_processes(struct qgis_project_s *proj, int signum)
{
    assert(proj);

    int children = 0;
    struct qgis_process_list_s *proclist = qgis_project_get_process_list(proj);
    int retval;
    int i;
    pid_t *pidlist = NULL;
    int pidlen = 0;
    retval = qgis_process_list_get_pid_list(proclist, &pidlist, &pidlen);
    for(i=0; i<pidlen; i++)
    {
	pid_t pid = pidlist[i];
	retval = kill(pid, signum);
	if (0 > retval)
	{
	    switch(errno)
	    {
	    case ESRCH:
		/* child process is not existent anymore.
		 * erase it from the list of available processes
		 */
	    {
		struct qgis_process_s *myproc = qgis_process_list_find_process_by_pid(proclist, pid);
		if (myproc)
		{
		    qgis_process_list_remove_process(proclist, myproc);
		    qgis_process_delete(myproc);
		}
	    }
	    break;
	    default:
		perror("error: could not send TERM signal");
	    }
	}
	else
	{
	    children++;
	}

    }
    free(pidlist);

    return children;
}


struct qgis_process_list_s *qgis_project_get_process_list(struct qgis_project_s *proj)
{
    struct qgis_process_list_s *list = NULL;

    assert(proj);
    if (proj)
    {
	list = proj->activeproclist;
    }

    return list;
}


const char *qgis_project_get_name(struct qgis_project_s *proj)
{
    const char *name = NULL;

    assert(proj);
    if (proj)
    {
	name = proj->name;
    }

    return name;
}


