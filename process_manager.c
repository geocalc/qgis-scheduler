/*
 * process_manager.c
 *
 *  Created on: 04.03.2016
 *      Author: jh
 */

/*
    Management module for the processes.
    Acts on events to maintenance the processes.

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



#include "process_manager.h"

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/types.h>
#include <errno.h>
#include <assert.h>
#include <fastcgi.h>
#include <pthread.h>

#include "database.h"
#include "fcgi_state.h"
#include "logger.h"
#include "qgis_config.h"
#include "qgis_shutdown_queue.h"
#include "project_manager.h"
#include "statistic.h"
#include "timer.h"


#define MIN_PROCESS_RUNTIME_SEC		5
#define MIN_PROCESS_RUNTIME_NANOSEC	0


struct thread_init_new_child_args
{
    pid_t pid;
    const char *project_name;
};


struct thread_start_new_child_args
{
    char *project_name;
};


struct thread_start_process_detached_args
{
    int num;
    char *project_name;
    int do_exchange_processes;
};



/* constants */
static const char base_socket_desc[] = "qgis-schedulerd-socket";
static const int max_nr_process_crashes = 5;
/* This global number is counted upwards, overflow included.
 * Every new unix socket gets this number attached creating a unique socket
 * path. In the very unlikely case the number is already given to an existing
 * socket path, the number is counted upwards again until we find a path which
 * is not assigned to a socket.
 * I assume here we do not create UINT_MAX number of sockets in this computer..
 */
static unsigned int socket_id = 0;
static pthread_mutex_t socket_id_mutex = PTHREAD_MUTEX_INITIALIZER;






static void process_manager_thread_function_init_new_child(struct thread_init_new_child_args *tinfo)
{
    assert(tinfo);
    const pid_t pid = tinfo->pid;
    assert(pid > 0);
    const char *projname = tinfo->project_name;
    assert(projname);
    const pthread_t thread_id = pthread_self();
    char *buffer;
    int retval;

    db_process_set_state_init(pid, thread_id);


    debug(1, "init new spawned child process for project '%s'", projname);


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
    int childunixsocketfd = db_get_process_socket(pid);

    retval = getsockname(childunixsocketfd, (struct sockaddr *)&sockaddr, &sockaddrlen);
    if (-1 == retval)
    {
	logerror("error retrieving the name of child process socket %d", childunixsocketfd);
	exit(EXIT_FAILURE);
    }
    /* leave the original child socket and create a new one on the opposite
     * side.
     * create the socket in blocking mode (non SOCK_NONBLOCK) because we need the
     * read() and write() calls waiting on it.
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
//    debug(1, "init project '%s', connected to child via socket '\\0%s'", projname, sockaddr.sun_path+1);


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
	debug(1, "fcgi message buffer too small (%d)", maxbufferlen);
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
	    debug(1, "Param %s=%s", key, value);

	    retval = fcgi_param_list_write(parambuffer, remain_len, key, value);
	    if (-1 == retval)
	    {
		// TODO: be more flexible if buffer too small
		debug(1, "fcgi parameter buffer too small (%d)", maxbufferlen);
		exit(EXIT_FAILURE);

	    }

	    parambuffer += retval;
	    remain_len -= retval;

	}
	len = maxbufferlen - remain_len;

	if (i>=128)
	{
	    debug(1, "fcgi parameter too many key/value pairs");
	    exit(EXIT_FAILURE);

	}
    }

    /* send parameter list */
    message = fcgi_message_new_parameter(requestid, buffer, len);
    len = fcgi_message_write(buffer, maxbufferlen, message);
    if (-1 == len)	// TODO: be more flexible if buffer too small
    {
	debug(1, "fcgi message buffer too small (%d)", maxbufferlen);
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
	debug(1, "fcgi message buffer too small (%d)", maxbufferlen);
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
	debug(1, "fcgi message buffer too small (%d)", maxbufferlen);
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
//	debug(1, "init project '%s' received:\n%.*s", projname, retval, buffer);
	if (-1 == retval)
	{
	    logerror("error: read() from child process during init phase");
	}
    }

    /* if the child process died during the initialization we need to figure
     * this out.
     * there may be a race condition between the signal handler and this thread
     * so we test the existence of the child process after the read.
     */
    retval = kill(pid, 0);
    if (-1 == retval)
    {
	if (ESRCH == errno)
	{
	    /* child process died during initialization.
	     * start a new one if possible
	     */
	    process_manager_process_died_during_init(pid, projname);
	}
	else
	{
	    logerror("error: kill(%d,0) returned", pid);
	    exit(EXIT_FAILURE);
	}
    }
    else
    {
	db_process_set_state_idle(pid);
    }

    /* ok, we did read each and every byte from child process.
     * now close this and set idle
     */
    retval = close(childunixsocketfd);
    debug(1, "closed child socket fd %d, retval %d, errno %d", childunixsocketfd, retval, errno);
//    close(debugfd);
    debug(1, "init child process for project '%s' done. waiting for input..", projname);


    free(buffer);
}


/* return the child process id if successful, 0 otherwise */
static int process_manager_thread_function_start_new_child(struct thread_start_new_child_args *tinfo)
{
    assert(tinfo);
    const char *project_name = tinfo->project_name;
    const char *command = config_get_process( project_name );

    debug(1, "project '%s' start new child process '%s'", project_name, command);

    if (NULL == command || 0 == strlen(command))
    {
	printlog("project '%s' error: no process path specified. Not starting any process", project_name);
	return 0;
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
	debug(1, "start project '%s', bound socket to '\\0%s'", project_name, childsockaddr.sun_path+1);
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


	/* Add the configured environment to the existing environment */
	/* Note: shall we clean up before? */
	int i;
	static const int maxenv = 25;
	for (i=0; i<maxenv; i++)
	{
	    const char *key = config_get_env_key(project_name, i);
	    if ( !key )
		break;
	    const char *value = config_get_env_value(project_name, i);
	    if ( !value )
		break;

	    debug(1, "project %s: add %s = %s to environment", project_name, key, value);
	    retval = setenv(key, value, 1);
	    if (retval)
	    {
		logerror("error can not set environment with key='%s' and value='%s'", key, value);
		exit(EXIT_FAILURE);
	    }
	}

	/* change working dir
	 * close file descriptor stdin = 0
	 * assign socket file descriptor to fd 0
	 * fork
	 * exec
	 */
	retval = chdir(config_get_working_directory(project_name));
	if (-1 == retval)
	{
	    logerror("error calling chdir");
	}


	retval = dup2(childsocket, FCGI_LISTENSOCK_FILENO);
	if (-1 == retval)
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
	debug(1, "project '%s' started new child process '%s', pid %d", project_name, command, pid);
	db_add_process( project_name, pid, childsocket);

	return pid;
    }
    else
    {
	/* error */
	logerror("can not fork");
	exit(EXIT_FAILURE);
    }

    return 0;
}


static void *process_manager_thread_start_new_child(void *arg)
{
    assert(arg);
    struct thread_start_new_child_args *tinfo = arg;
    struct thread_init_new_child_args initargs;
    struct timespec ts;

    qgis_timer_start(&ts);
    initargs.pid = process_manager_thread_function_start_new_child(arg);
#ifdef DISABLED_INIT
#warning disabled init phase
    qgis_process_set_state_idle(initargs.proc);
#else
    if (initargs.pid > 0)
    {
	initargs.project_name = tinfo->project_name;
	process_manager_thread_function_init_new_child(&initargs);
    }
#endif
    qgis_timer_stop(&ts);
    printlog("Startup time for project '%s' %ld.%03ld sec", tinfo->project_name, ts.tv_sec, ts.tv_nsec/(1000*1000));

    free(tinfo->project_name);
    free(arg);
    return NULL;
}


/* starts "num" new child processes synchronously.
 * param num: number of processes to start (num>=0)
 * param project: project to manage them
 * param do_exchange_processes: if true removes all active processes and replaces them with the new created ones.
 *                              else integrate them in the list of active processes.
 */
void process_manager_start_new_process_wait(int num, const char *projname, int do_exchange_processes)
{
    assert(projname);
    assert(num > 0);

    pthread_t threads[num];
    int i;
    int retval;

    printlog("Starting %d process%s for project '%s'", num, (num>1)?"es":"", projname);

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
	targs->project_name = strdup(projname);

	retval = pthread_create(&threads[i], NULL, process_manager_thread_start_new_child, targs);
	if (retval)
	{
	    errno = retval;
	    logerror("error creating thread");
	    exit(EXIT_FAILURE);
	}
	debug(1, "[%lu] started thread %lu", pthread_self(), threads[i]);
    }
    /* wait for those threads */
    for (i=0; i<num; i++)
    {
	debug(1, "[%lu] join thread %lu", pthread_self(), threads[i]);
	retval = pthread_join(threads[i], NULL);
	if (retval)
	{
	    errno = retval;
	    logerror("error joining thread");
	    exit(EXIT_FAILURE);
	}
    }


    /* move the processes from the initialization list to the active process
     * list.
     * If we got the option to exchange the processes then first move all
     * existing processes from the active list to the shutdown queue.
     *
     * Note: The option to exchange the processes is usually set if a new
     * configuration file has been copied to the processes.
     * If a new configuration file arrives the number of crashed processes is
     * reset. But if we do this during a crashing process, the number becomes
     * invalid. So we can not reset the number in
     * qgis_project_check_inotify_config_changed(), because it is not
     * protected. We have the reset the number over here.
     */
    if (do_exchange_processes)
    {
	db_move_all_process_from_active_to_shutdown_list(projname);
	db_reset_startup_failures(projname);	// TODO: move this line to the config change manager
    }

    db_move_all_idle_process_from_init_to_active_list(projname);

    statistic_add_process_start(num);

}


static void *process_manager_thread_start_process_detached(void *arg)
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

    process_manager_start_new_process_wait(tinfo->num, tinfo->project_name, tinfo->do_exchange_processes);

    free(tinfo->project_name);
    free(arg);

    return NULL;
}


/* starts a new thread in detached state, which in turn calls start_new_process_wait() */
void process_manager_start_new_process_detached(int num, const char *projname, int do_exchange_processes)
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
    targs->project_name = strdup(projname);
    targs->do_exchange_processes = do_exchange_processes;

    pthread_t thread;
    int retval = pthread_create(&thread, NULL, process_manager_thread_start_process_detached, targs);
    if (retval)
    {
	errno = retval;
	logerror("error: pthread_create");
	exit(EXIT_FAILURE);
    }

}



/* a child process died.
 * this may happen because we cancelled its operation or
 * the process died because of a bug or low memory or something else.
 * Get the project this process was tasked for. If there is no project then the
 * process was already scheduled to shut down or the process id did not belong
 * to us. Either way in this case we don't need to take action.
 * Else look for the number of idle processes and restart a new process if
 * needed.
 */
void process_manager_process_died(pid_t pid)
{
    int retval = db_has_process(pid);
    if (retval)
    {
	/* check if we are during shutdown sequence. if not then restart the
	 * process.
	 * check if the process died during the initialisation sequence
	 * if it did, remark the process being instable.
	 * then start a new one.
	 * Refrain from restarting if too much processes have died during the
	 * initialization.
	 */
	retval = get_program_shutdown();
	if (!retval)
	{
	    char *projname = db_get_project_for_this_process(pid);
//	    enum db_process_list_e proclist = db_get_process_list(pid);
//	    if (LIST_INIT == proclist)
//	    {
//		/* died during initialization. if this happens every time,
//		 * we do get a startup loop. stop after some (5) tries.
//		 */
//		db_inc_startup_failures(projname);
//	    }
	    if (projname)
	    {
		retval = db_get_startup_failures(projname);
		if ( 0 > retval )
		{
		    printlog("error: can not get number of startup failures, function call failed for project %s", projname);
		    exit(EXIT_FAILURE);
		}
		else if (max_nr_process_crashes > retval)
		{
		    project_manager_start_new_process_detached(1, projname, 0);
		}
		free(projname);
	    }
	    else
	    {
		printlog("warning: no project found for pid %d", pid);
	    }
	}

	/* change state of the process to STATE_EXIT
	 * and move the entry to the shutdown list
	 */
	db_process_set_state_exit(pid);
	qgis_shutdown_add_process(pid);
    }
    else
    {
	printlog("got signal SIGCHLD but pid %d does not belong to us", pid);
    }
}


/* like process_manager_process_died() but without intensive tests */
void process_manager_process_died_during_init(pid_t pid, const char *projname)
{
    int retval = get_program_shutdown();
    if (!retval)
    {
	db_inc_startup_failures(projname);

//	retval = db_get_startup_failures(projname);
//	if (max_nr_process_crashes > retval)
//	{
//	    project_manager_start_new_process_detached(1, projname, 0);
//	}
    }

    /* change state of the process to STATE_EXIT
     * and move the entry to the shutdown list
     */
//    db_process_set_state_exit(pid);
//    qgis_shutdown_add_process(pid);
}


void process_manager_cleanup_process(pid_t pid)
{
    assert(0 < pid);

    int fd = db_get_process_socket(pid);
    if (-1 == fd)
	printlog("error: can not get socket fd from process %d during cleanup", pid);
    else
	close(fd);
    db_process_set_state_exit(pid);
}


