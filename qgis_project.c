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
#include "qgis_inotify.h"
#include "fcgi_state.h"
#include "logger.h"
#include "timer.h"


//#define DISABLED_INIT
#define MIN_PROCESS_RUNTIME_SEC		5
#define MIN_PROCESS_RUNTIME_NANOSEC	0



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
//    int inotifyfd;
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
	start_new_process_detached(numproc, project, 1);
    }
}


/* checks if the file name and watch descriptor belong to this project
 * initiate a process restart if the config did change.
 */
int qgis_project_check_inotify_config_changed(struct qgis_project_s *project, int wd)
{
    int ret = 0;

    if (wd == project->inotifywatchfd)
    {
	ret = 1;

	/* match, start new processes and then move them to idle list */
	printlog("Project '%s' config change. Restart processes", project->name);
	qgis_project_restart_processes(project);
    }

    return ret;
}


struct qgis_project_s *qgis_project_new(const char *name, const char *configpath)
{
    assert(name);
    //assert(configpath); configpath is allowed to be NULL. in this case no watcher thread is started

    struct qgis_project_s *proj = calloc(1, sizeof(*proj));
    assert(proj);
    if ( !proj )
    {
	logerror("could not allocate memory");
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
	logerror("error init read-write lock");
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
		logerror("error accessing file '%s': ", configpath);
		debug(1, "file is not watched for changes\n");
		break;

	    default:
		logerror("error accessing file '%s': ", configpath);
		exit(EXIT_FAILURE);
	    }
	}
	else
	{
	    if (S_ISREG(statbuf.st_mode))
	    {
		/* if I can stat the file I assume we can read it.
		 * Now setup the inotify descriptor.
		 */
		retval = qgis_inotify_watch_file(configpath);
		proj->inotifywatchfd = retval;

		proj->configbasename = basename((char *)configpath);
		proj->configpath = configpath;

	    }
	    else
	    {
		debug(1, "error '%s' is no regular file\n", configpath);
	    }
	}
    }

    return proj;
}


void qgis_project_delete(struct qgis_project_s *proj)
{
    if (proj)
    {
	debug(1, "deleting project '%s'\n", proj->name);

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


    debug(1, "init new spawned child process for project '%s'\n", projname);


//    char debugfile[256];
//    sprintf(debugfile, "/tmp/threadinit.%lu.dump", thread_id);
//    int debugfd = open(debugfile, (O_WRONLY|O_CREAT|O_TRUNC), (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH));
//    if (-1 == debugfd)
//    {
//	debug(1, "error can not open file '%s': ", debugfile);
//	logerror(NULL);
//	exit(EXIT_FAILURE);
//    }


    /* open the socket to the child process */

    struct sockaddr_un sockaddr;
    socklen_t sockaddrlen = sizeof(sockaddr);
    int childunixsocketfd = qgis_process_get_socketfd(childproc);

    retval = getsockname(childunixsocketfd, (struct sockaddr *)&sockaddr, &sockaddrlen);
    if (-1 == retval)
    {
	logerror("error retrieving the name of child process socket");
	exit(EXIT_FAILURE);
    }
    /* leave the original child socket and create a new one on the opposite
     * side.
     */
    retval = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (-1 == retval)
    {
	logerror("error: can not create socket to child process");
	exit(EXIT_FAILURE);
    }
    childunixsocketfd = retval;	// refers to the socket this program connects to the child process
    retval = connect(childunixsocketfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));
    if (-1 == retval)
    {
	logerror("error: can not connect to child process");
	exit(EXIT_FAILURE);
    }
//    debug(1, "init project '%s', connected to child via socket '\\0%s'\n", projname, sockaddr.sun_path+1);


    /* create the fcgi data and
     * send the fcgi data to the child process
     */
    static const int maxbufferlen = 4096;
    buffer = malloc(maxbufferlen);
    assert(buffer);
    if ( !buffer )
    {
	logerror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    static const int requestid = 1;
    int len;
    struct fcgi_message_s *message = fcgi_message_new_begin(requestid, FCGI_RESPONDER, 0);
    len = fcgi_message_write(buffer, maxbufferlen, message);
    if (-1 == len)	// TODO: be more flexible if buffer too small
    {
	debug(1, "fcgi message buffer too small (%d)\n", maxbufferlen);
	exit(EXIT_FAILURE);
    }
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	logerror("error: can not write to child process");
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
	    debug(1, "Param %s=%s\n", key, value);

	    retval = fcgi_param_list_write(parambuffer, remain_len, key, value);
	    if (-1 == retval)
	    {
		// TODO: be more flexible if buffer too small
		debug(1, "fcgi parameter buffer too small (%d)\n", maxbufferlen);
		exit(EXIT_FAILURE);

	    }

	    parambuffer += retval;
	    remain_len -= retval;

	}
	len = maxbufferlen - remain_len;

	if (i>=128)
	{
	    debug(1, "fcgi parameter too many key/value pairs\n");
	    exit(EXIT_FAILURE);

	}
    }

    /* send parameter list */
    message = fcgi_message_new_parameter(requestid, buffer, len);
    len = fcgi_message_write(buffer, maxbufferlen, message);
    if (-1 == len)	// TODO: be more flexible if buffer too small
    {
	debug(1, "fcgi message buffer too small (%d)\n", maxbufferlen);
	exit(EXIT_FAILURE);
    }
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	logerror("error: can not write to child process");
	exit(EXIT_FAILURE);
    }
    fcgi_message_delete(message);

    /* send empty parameter list to signal EOP */
    message = fcgi_message_new_parameter(requestid, "", 0);
    len = fcgi_message_write(buffer, maxbufferlen, message);
    if (-1 == len)	// TODO: be more flexible if buffer too small
    {
	debug(1, "fcgi message buffer too small (%d)\n", maxbufferlen);
	exit(EXIT_FAILURE);
    }
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	logerror("error: can not write to child process");
	exit(EXIT_FAILURE);
    }
    fcgi_message_delete(message);


    message = fcgi_message_new_stdin(requestid, "", 0);
    len = fcgi_message_write(buffer, maxbufferlen, message);
    if (-1 == len)
    {
	debug(1, "fcgi message buffer too small (%d)\n", maxbufferlen);
	exit(EXIT_FAILURE);
    }
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	logerror("error: can not write to child process");
	exit(EXIT_FAILURE);
    }
    // write stdin = "" twice
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	logerror("error: can not write to child process");
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
//	debug(1, "init project '%s' received:\n%.*s\n", projname, retval, buffer);
    }


    /* ok, we did read each and every byte from child process.
     * now close this and set idle
     */
    close(childunixsocketfd);
//    close(debugfd);
    debug(1, "init child process for project '%s' done. waiting for input..\n", projname);

    // TODO: do we need this distinction over here?
    if (childproc)
    {
	qgis_process_set_state_idle(childproc);
    }
    else
    {
	debug(1, "no child process found to initialize\n");
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

    debug(1, "project '%s' start new child process '%s'\n", project_name, command);

    if (NULL == command || 0 == strlen(command))
    {
	printlog("project '%s' error: no process path specified. Not starting any process", project_name);
	return NULL;
    }

    /* prepare the socket to connect to this child process only */
    /* NOTE: Linux allows abstract socket names which have no representation
     * in the filesystem namespace.
     */
    int retval = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (-1 == retval)
    {
	logerror("can not create socket for fcgi program");
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
	logerror("error acquire mutex");
	exit(EXIT_FAILURE);
    }
    unsigned int socket_suffix_start = socket_id-1;
    retval = pthread_mutex_unlock (&socket_id_mutex);
    if (retval)
    {
	errno = retval;
	logerror("error unlock mutex");
	exit(EXIT_FAILURE);
    }

    for (;;)
    {
	retval = pthread_mutex_lock (&socket_id_mutex);
	if (retval)
	{
	    errno = retval;
	    logerror("error acquire mutex");
	    exit(EXIT_FAILURE);
	}
	unsigned int socket_suffix = socket_id++;
	retval = pthread_mutex_unlock (&socket_id_mutex);
	if (retval)
	{
	    errno = retval;
	    logerror("error unlock mutex");
	    exit(EXIT_FAILURE);
	}

	if (socket_suffix == socket_suffix_start)
	{
	    /* we tested UINT_MAX numbers without success.
	     * exit here, because we can not get any more numbers.
	     * Or we have a programmers error here..
	     */
	    debug(1, "error: out of numbers to create socket name. exit");
	    exit(EXIT_FAILURE);
	}

	struct sockaddr_un childsockaddr;
	childsockaddr.sun_family = AF_UNIX;
	retval = snprintf( childsockaddr.sun_path, sizeof(childsockaddr.sun_path), "%c%s%u", '\0', base_socket_desc, socket_suffix );
	if (-1 == retval)
	{
	    logerror("error calling string format function snprintf");
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
		logerror("error calling bind");
		exit(EXIT_FAILURE);
	    }
	}
	debug(1, "start project '%s', bound socket to '\\0%s'\n", project_name, childsockaddr.sun_path+1);
	break;
    }

    /* the child process listens for connections, one at a time */
    retval = listen(childsocket, 1);
    if (-1 == retval)
    {
	logerror("can not listen to socket connecting fast cgi application");
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
	    logerror("error calling chdir");
	}


	ret = dup2(childsocket, FCGI_LISTENSOCK_FILENO);
	if (-1 == ret)
	{
	    logerror("error calling dup2");
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
	logerror("could not execute '%s': ", command);
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
	logerror("can not fork");
	exit(EXIT_FAILURE);
    }

    return NULL;
}

void *thread_start_new_child(void *arg)
{
    assert(arg);
    struct thread_start_new_child_args *tinfo = arg;
    struct thread_init_new_child_args initargs;
    struct timespec ts;

    qgis_timer_start(&ts);
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
    qgis_timer_stop(&ts);
    const char *projname = qgis_project_get_name(tinfo->project);
    printlog("Startup time for project '%s' %ld.%03ld sec", projname, ts.tv_sec, ts.tv_nsec/(1000*1000));

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
    assert(project);
    assert(num > 0);

    pthread_t threads[num];
    int i;
    int retval;

    printlog("Starting %d process%s for project '%s'", num, (num>1)?"es":"", project->name);

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
	    logerror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	targs->project = project;

	retval = pthread_create(&threads[i], NULL, thread_start_new_child, targs);
	if (retval)
	{
	    errno = retval;
	    logerror("error creating thread");
	    exit(EXIT_FAILURE);
	}
	debug(1, "[%lu] started process %lu\n", pthread_self(), threads[i]);
    }
    /* wait for those threads */
    for (i=0; i<num; i++)
    {
	debug(1, "[%lu] join process %lu\n", pthread_self(), threads[i]);
	retval = pthread_join(threads[i], NULL);
	if (retval)
	{
	    errno = retval;
	    logerror("error joining thread");
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
	logerror("error acquire read-write lock");
	exit(EXIT_FAILURE);
    }

    if (do_exchange_processes)
    {
	retval = qgis_process_list_transfer_all_process(project->shutdownproclist, project->activeproclist);
	debug(1, "project '%s' moved %d processes from active list to shutdown list\n", project->name, retval);
	retval = qgis_process_list_send_signal(project->shutdownproclist, SIGTERM);
	debug(1, "project '%s' send %d processes the TERM signal\n", project->name, retval);
    }
    retval = qgis_process_list_transfer_all_process_with_state(project->activeproclist, project->initproclist, PROC_IDLE);
    debug(1, "project '%s' moved %d processes from init list to active list\n", project->name, retval);

    retval = pthread_rwlock_unlock(&project->rwlock);
    if (retval)
    {
	errno = retval;
	logerror("error unlock read-write lock");
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
	logerror("error detaching thread");
	exit(EXIT_FAILURE);
    }

    start_new_process_wait(tinfo->num, tinfo->project, tinfo->do_exchange_processes);

    free(arg);

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
	logerror("could not allocate memory");
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
	logerror("error: pthread_create");
	exit(EXIT_FAILURE);
    }

}



/* some process died and sent its pid via SIGCHLD.
 * maybe it was listed in this project?
 * test and remove the old entry and maybe restart anew.
 * test all three lists initproclist, activeproclist and shutdownproclist.
 */
int qgis_project_process_died(struct qgis_project_s *proj, pid_t pid)
{
    assert(proj);
    assert(pid>0);

    int ret = 0;

    if (proj)
    {
	/* Erase the old entry. The process does not exist anymore */
	struct qgis_process_list_s *proclist = proj->activeproclist;
	assert(proclist);
	struct qgis_process_s *proc = qgis_process_list_find_process_by_pid(proclist, pid);
	if (proc)
	{
	    ret++;
	    /* that process belongs to our active list.
	     * restart the process if not during shutdown.
	     */
	    struct timespec ts = *qgis_process_get_starttime(proc);
	    int retval = qgis_timer_stop(&ts);
	    if (-1 == retval)
	    {
		logerror("clock_gettime(%d,..)", get_valid_clock_id());
		exit(EXIT_FAILURE);
	    }
	    if ( MIN_PROCESS_RUNTIME_SEC > ts.tv_sec || (MIN_PROCESS_RUNTIME_SEC == ts.tv_sec && MIN_PROCESS_RUNTIME_NANOSEC >= ts.tv_nsec) )
		printlog("WARNING: Process %d died within %ld.%03ld sec", pid, ts.tv_sec, ts.tv_nsec/(1000*1000));

	    qgis_process_list_remove_process(proclist, proc);
	    qgis_process_delete(proc);

	    if ( !get_program_shutdown() )
	    {
		debug(1, "project '%s' restarting process\n", proj->name);
		printlog("Project '%s', process %d died, restarting process", proj->name, pid);

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
		ret++;
		/* that process belongs to our active list.
		 * restart the process if not during shutdown.
		 */
		struct timespec ts = *qgis_process_get_starttime(proc);
		int retval = qgis_timer_stop(&ts);
		if (-1 == retval)
		{
		    logerror("clock_gettime(%d,..)", get_valid_clock_id());
		    exit(EXIT_FAILURE);
		}
		if ( MIN_PROCESS_RUNTIME_SEC > ts.tv_sec || (MIN_PROCESS_RUNTIME_SEC == ts.tv_sec && MIN_PROCESS_RUNTIME_NANOSEC >= ts.tv_nsec) )
		    printlog("WARNING: Process %d died within %ld.%03ld sec", pid, ts.tv_sec, ts.tv_nsec/(1000*1000));

		qgis_process_list_remove_process(proclist, proc);
		qgis_process_delete(proc);

		if ( !get_program_shutdown() )
		{
		    debug(1, "project '%s' restarting process\n", proj->name);
		    printlog("Project '%s', process %d died, restarting process", proj->name, pid);

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
			ret++;
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

    return ret;
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
		logerror("error: could not send TERM signal");
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


