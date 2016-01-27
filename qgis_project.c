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
#include <fastcgi.h>

#include "qgis_process_list.h"
#include "qgis_config.h"
#include "fcgi_state.h"


struct qgis_project_s
{
    struct qgis_process_list_s *proclist;	// list of processes which handle fcgi requests
    const char *name;
    const char *configpath;		// path to qgis config file watched for updates. note: no watch if configpath is NULL
    pthread_t config_watch_threadid;
    int does_shutdown;
    struct qgis_process_list_s *shutdownproclist; // list of processes which are shut down, no need to restart them
    pthread_rwlock_t rwlock;
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




/* does restart all processes belonging to this project.
 * We move the list of processes from proclist to shutdownproclist.
 * Then we start the required number of processes and initialize them.
 * The required number may be the number of processes from the old list
 * or the configuration parameter for minimal free idle processes.
 */
//static void qgis_project_restart_processes(struct qgis_project_s *proj)
//{
//    assert(proj);
//
//    int retval = pthread_rwlock_wrlock(&proj->rwlock);
//    if (retval)
//    {
//	errno = retval;
//	perror("error acquire read-write lock");
//	exit(EXIT_FAILURE);
//    }
//
//    if ( !proj->shutdownproclist )
//	proj->shutdownproclist = qgis_process_list_new();
//
//    qgis_process_list_transfer_all_process(proj->shutdownproclist, proj->proclist);
//
//    retval = pthread_rwlock_unlock(&proj->rwlock);
//    if (retval)
//    {
//	errno = retval;
//	perror("error unlock read-write lock");
//	exit(EXIT_FAILURE);
//    }
//
//}





/* watches changes in the configuration file.
 * uses inotify.
 */
//static void *thread_watch_config(void *arg)
//{
//
//    return NULL;
//}






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
    proj->proclist = qgis_process_list_new();
    proj->name = name;
    proj->configpath = configpath;

    return proj;
}


void qgis_project_delete(struct qgis_project_s *proj)
{
    if (proj)
    {
	qgis_process_list_delete(proj->proclist);
	free(proj);
    }
}


//int qgis_project_add_process_list(struct qgis_project_s *proj, struct qgis_process_list_s *proclist)
//{
//    assert(proj);
//    assert(proclist);
//    if (proj && proclist)
//    {
//	proj->proclist = proclist;
//
//	return 0;
//    }
//
//    return -1;
//}


int qgis_project_add_process(struct qgis_project_s *proj, struct qgis_process_s *proc)
{
    assert(proj);
    assert(proc);
    assert(proj->proclist);
    if (proj && proj->proclist && proc)
    {
	qgis_process_list_add_process(proj->proclist, proc);

	return 0;
    }

    return -1;
}


void *thread_init_new_child(void *arg)
{
    assert(arg);
    struct thread_init_new_child_args *tinfo = arg;
    struct qgis_process_s *childproc = tinfo->proc;
    assert(childproc);
    const char *projname = tinfo->project_name;
    assert(projname);
    const pthread_t thread_id = pthread_self();
    char *buffer ;

    qgis_process_set_state_init(childproc, thread_id);

    /* detach myself from the main thread. Doing this to collect resources after
     * this thread ends. Because there is no join() waiting for this thread.
     */
    int retval = pthread_detach(thread_id);
    if (-1 == retval)
    {
	perror("error detaching thread");
	exit(EXIT_FAILURE);
    }

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
    free(arg);

    return NULL;
}



void *thread_start_new_child(void *arg)
{
    assert(arg);
    struct thread_start_new_child_args *tinfo = arg;
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

	/* NOTE: aside from the general rule
	 * "malloc() and free() within the same function"
	 * we transfer the responsibility for this memory
	 * to the thread itself.
	 */
	struct thread_init_new_child_args *targs = malloc(sizeof(*targs));
	assert(targs);
	if ( !targs )
	{
	    perror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	targs->proc = childproc;
	targs->project_name = project_name;

	// TODO: move start and init threads to functions, call both with one thread, wait for init phase during scheduler startup.
	pthread_t thread;
	retval = pthread_create(&thread, NULL, thread_init_new_child, targs);
	if (retval)
	{
	    errno = retval;
	    perror("error creating thread");
	    exit(EXIT_FAILURE);
	}

    }
    else
    {
	/* error */
	perror("can not fork");
	exit(EXIT_FAILURE);
    }

    free(arg);
    return NULL;
}

void start_new_process_wait(int num, struct qgis_project_s *project)
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
    }
    /* wait for those threads */
    for (i=0; i<num; i++)
    {
	retval = pthread_join(threads[i], NULL);
	if (retval)
	{
	    errno = retval;
	    perror("error joining thread");
	    exit(EXIT_FAILURE);
	}
    }

}



void start_new_process_detached(int num, struct qgis_project_s *project)
{
    /* Start the process creation thread in detached state because
     * we do not want to wait for it. Different from the handling
     * during the program startup there is no join() waiting for
     * the end of the thread and collecting its resources.
     */
    pthread_t thread;
    pthread_attr_t thread_attr;

    int retval = pthread_attr_init(&thread_attr);
    if (retval)
    {
	errno = retval;
	perror("error: pthread_attr_init");
	exit(EXIT_FAILURE);
    }
    retval = pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
    if (retval)
    {
	errno = retval;
	perror("error: pthread_attr_setdetachstate PTHREAD_CREATE_DETACHED");
	exit(EXIT_FAILURE);
    }
    while (num-- > 0)
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

	retval = pthread_create(&thread, &thread_attr, thread_start_new_child, targs);
	if (retval)
	{
	    errno = retval;
	    perror("error: pthread_create");
	    exit(EXIT_FAILURE);
	}
    }
    retval = pthread_attr_destroy(&thread_attr);
    if (retval)
    {
	errno = retval;
	perror("error: pthread_attr_destroy");
	exit(EXIT_FAILURE);
    }
}



/* some process died and sent its pid via SIGCHLD.
 * maybe it was listet in this project?
 * test and remove the old entry and maybe restart anew.
 */
void qgis_project_process_died(struct qgis_project_s *proj, pid_t pid)
{
    assert(proj);
    assert(pid>0);

    if (proj)
    {
	/* Erase the old entry. The process does not exist anymore */
	struct qgis_process_list_s *proclist = proj->proclist;
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
		fprintf(stderr, "restarting process\n");

		/* child process terminated, restart anew */
		/* TODO: react on child processes exiting immediately.
		 * maybe store the creation time and calculate the execution time?
		 */
		start_new_process_detached(1, proj);
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


int qgis_project_shutdown_process(struct qgis_project_s *proj, struct qgis_process_s *proc)
{

    return -1;
}


int qgis_project_shutdown_all_process(struct qgis_project_s *proj)
{

    return -1;
}


struct qgis_process_list_s *qgis_project_get_process_list(struct qgis_project_s *proj)
{
    struct qgis_process_list_s *list = NULL;

    assert(proj);
    if (proj)
    {
	list = proj->proclist;
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


