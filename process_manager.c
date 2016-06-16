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
#include <poll.h>

#include "database.h"
#include "fcgi_state.h"
#include "logger.h"
#include "qgis_config.h"
#include "qgis_shutdown_queue.h"
#include "project_manager.h"
#include "statistic.h"
#include "stringext.h"
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


static ssize_t read_timeout(int filedes, void *buffer, size_t size, int timeout_ms)
{
    ssize_t ret = -1;

    struct pollfd pfd;

    pfd.fd = filedes;
    pfd.events = POLLIN;

    int retval = poll(&pfd, 1, timeout_ms);
    if (retval > 0)
	// file descriptor ready, read from it
	ret = read(filedes, buffer, size);
    else if (!retval)
	// timeout, mark as error with ETIMEDOUT and return -1
	errno = ETIMEDOUT;
    // else error happened, just return error with errno

    return ret;
}



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
//	debug(1, "ERROR: can not open file '%s': ", debugfile);
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
	logerror("ERROR: retrieving the name of child process socket %d", childunixsocketfd);
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
	logerror("ERROR: can not create socket to child process");
	exit(EXIT_FAILURE);
    }
    childunixsocketfd = retval;	// refers to the socket this program connects to the child process
    retval = connect(childunixsocketfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));
    if (-1 == retval)
    {
	logerror("ERROR: init can not connect to child process");
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
	logerror("ERROR: could not allocate memory");
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
	logerror("ERROR: can not write to child process");
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
	logerror("ERROR: can not write to child process");
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
	logerror("ERROR: can not write to child process");
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
	logerror("ERROR: can not write to child process");
	exit(EXIT_FAILURE);
    }
    // write stdin = "" twice
//    retval = write(debugfd, buffer, len);
    retval = write(childunixsocketfd, buffer, len);
    if (-1 == retval)
    {
	logerror("ERROR: can not write to child process");
	exit(EXIT_FAILURE);
    }
    fcgi_message_delete(message);


    /* now read from socket into void until no more data
     * we do it to make sure that the child process has completed the request
     * and filled up its cache.
     *
     * Set a timeout of N seconds in case the program crashed during start. If
     * the timeout catches move the process to the shutdown module and in the
     * database mark the process as crashed.
     */
    const int init_read_timeout = config_get_read_timeout(projname);
    int has_timeout = 0;
    retval = 1;
    while (retval>0)
    {
	retval = read_timeout(childunixsocketfd, buffer, maxbufferlen, init_read_timeout);
//	debug(1, "init project '%s' received:\n%.*s", projname, retval, buffer);
	if (-1 == retval)
	{
	    logerror("ERROR: read() from child process during init phase");
	    if (ETIMEDOUT == errno)
	    {
		has_timeout = 1;
	    }
	}
    }

    /* if the child process died during the initialization we need to figure
     * this out.
     * there may be a race condition between the signal handler and this thread
     * so we test the existence of the child process after the read.
     */
    if (has_timeout)
    {
	printlog("starting new process for project %s", projname);
	qgis_shutdown_add_process(pid);
	process_manager_process_died_during_init(pid, projname);
    }
    else
    {
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
		logerror("ERROR: kill(%d,0) returned", pid);
		exit(EXIT_FAILURE);
	    }
	}
	else
	{
	    db_process_set_state_idle(pid);
	}
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
	printlog("ERROR: no process path specified. Not starting any process for project '%s'", project_name);
	return 0;
    }

    /* prepare the socket to connect to this child process only */
    /* NOTE: Linux allows abstract socket names which have no representation
     * in the filesystem namespace.
     */
    int retval = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (-1 == retval)
    {
	logerror("ERROR: can not create socket for fcgi program");
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
	logerror("ERROR: acquire mutex");
	exit(EXIT_FAILURE);
    }
    unsigned int socket_suffix_start = socket_id-1;
    retval = pthread_mutex_unlock (&socket_id_mutex);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex");
	exit(EXIT_FAILURE);
    }

    for (;;)
    {
	retval = pthread_mutex_lock (&socket_id_mutex);
	if (retval)
	{
	    errno = retval;
	    logerror("ERROR: acquire mutex");
	    exit(EXIT_FAILURE);
	}
	unsigned int socket_suffix = socket_id++;
	retval = pthread_mutex_unlock (&socket_id_mutex);
	if (retval)
	{
	    errno = retval;
	    logerror("ERROR: unlock mutex");
	    exit(EXIT_FAILURE);
	}

	if (socket_suffix == socket_suffix_start)
	{
	    /* we tested UINT_MAX numbers without success.
	     * exit here, because we can not get any more numbers.
	     * Or we have a programmers error here..
	     */
	    debug(1, "ERROR: out of numbers to create socket name. exit");
	    exit(EXIT_FAILURE);
	}

	struct sockaddr_un childsockaddr;
	childsockaddr.sun_family = AF_UNIX;
	retval = snprintf( childsockaddr.sun_path, sizeof(childsockaddr.sun_path), "%c%s%u", '\0', base_socket_desc, socket_suffix );
	if (-1 == retval)
	{
	    logerror("ERROR: calling string format function snprintf");
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
		logerror("ERROR: calling bind");
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
	logerror("ERROR: can not listen to socket connecting fast cgi application");
	exit(EXIT_FAILURE);
    }


    /* preparation of data before fork() so we don't need to call functions with
     * mutexes after fork() (see below for further reading).
     */
    static const int maxenv = 128;	// to prevent infinite loop support 128 environment variables at max
    int lenkey = 0;
    int numkey = 0;
    const char **keys = NULL;
    const char **values = NULL;
    int i;
    for (i=0; i<maxenv; i++)
    {
	const char *key = config_get_env_key(project_name, i);
	if ( !key )
	    break;
	const char *value = config_get_env_value(project_name, i);
	if ( !value )
	    break;

	int lenvalue = lenkey;
	int numvalue = numkey;
	arraycat(&values, &numvalue, &lenvalue, &value, sizeof(*values) );
	arraycat(&keys, &numkey, &lenkey, &key, sizeof(*keys) );
	debug(1, "project %s: add %s = %s to environment", project_name, key, value);
    }

    const char *working_directory = config_get_working_directory(project_name);


    pid_t pid = fork();
    if (0 == pid)
    {
	/* child */

	/* according to
	 * http://www.linuxprogrammingblog.com/threads-and-fork-think-twice-before-using-them
	 * we may only call async-safe functions in multithreaded programs (this!)
	 * after calling fork() (here!).
	 *
	 * So we are not allowed to call ANY function with in turn calls locks
	 * or mutexes. We should only call functions which are listed as async-safe
	 * in signal(7).
	 * NOTE: setenv() is not listed as async-safe. Unfortunately I don't see
	 * any other safe way to prepare the environment for the child only?
	 * Maybe kill all threads (except this) with a combination of
	 * pthread_atfork() and pthread_cancel() ?
	 */

	/* Add the configured environment to the existing environment */
	/* Note: shall we clean up before? */
	for (i=0; i<lenkey; i++)
	{
//	    debug(1, "project %s: add %s = %s to environment", project_name, key, value); # no debug message allowed because of locking
	    retval = setenv(keys[i], values[i], 1);
	    if (retval)
	    {
//		logerror("ERROR: can not set environment with key='%s' and value='%s'", key[i], value[i]); # no log message allowed because of locking
		exit(EXIT_FAILURE);
	    }
	}
	// no need to free() memory, is freed by exec()

	/* change working dir
	 * close file descriptor stdin = 0
	 * assign socket file descriptor to fd 0
	 * fork
	 * exec
	 */
	retval = chdir(working_directory);
	if (-1 == retval)
	{
//	    logerror("ERROR: calling chdir"); # no log message allowed because of locking
	}


	retval = dup2(childsocket, FCGI_LISTENSOCK_FILENO);
	if (-1 == retval)
	{
//	    logerror("ERROR: calling dup2"); # no log message allowed because of locking
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
//	logerror("ERROR: could not execute '%s': ", command); # no log message allowed because of locking
	exit(EXIT_FAILURE);
    }
    else if (0 < pid)
    {
	/* parent */
	free(keys);
	free(values);
	debug(1, "project '%s' started new child process '%s', pid %d", project_name, command, pid);
	db_add_process( project_name, pid, childsocket);

	return pid;
    }
    else
    {
	/* error */
	logerror("ERROR: can not fork");
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
	    logerror("ERROR: could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	targs->project_name = strdup(projname);

	retval = pthread_create(&threads[i], NULL, process_manager_thread_start_new_child, targs);
	if (retval)
	{
	    errno = retval;
	    logerror("ERROR: creating thread");
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
	    logerror("ERROR: joining thread");
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
	// TODO: move only those processes which have been started above
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
	logerror("ERROR: detaching thread");
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
	logerror("ERROR: could not allocate memory");
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
	logerror("ERROR: pthread_create");
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
 *
 * This function is called from the signal handler.
 * If the signal handler receives a SIGCHLD there may be more than one child
 * process having send a signal. We have to test all programs if they still
 * exist in RAM.
 */
void process_manager_process_died(void)
{
    int retval;
    pid_t *pidlist;
    int listlen;
    int i;

    retval = db_get_complete_list_process(&pidlist, &listlen);
    for(i=0; i<listlen; i++)
    {
	/* check if we are during shutdown sequence. if not then restart the
	 * process.
	 * check if the process died during the initialisation sequence
	 * if it did, remark the process being instable.
	 * then start a new one.
	 * Refrain from restarting if too much processes have died during the
	 * initialization.
	 */
	const pid_t pid = pidlist[i];

	retval = kill(pid, 0);
	if (-1 == retval)
	{
	    if (ESRCH == errno)
	    {
		/* child process died.
		 */
		retval = get_program_shutdown();
		if (!retval)
		{
		    enum db_process_list_e proclist = db_get_process_list(pid);
		    if (LIST_SHUTDOWN != proclist)
		    {
			char *projname = db_get_project_for_this_process(pid);
			/* Process is not in shutdown list and died during normal operation.
			 * Restart the process if not too much startup failures
			 */
			if (projname)
			{
			    retval = db_get_startup_failures(projname);
			    if ( 0 > retval )
			    {	// too much dying processes during init phase, do not start new processes
				printlog("ERROR: can not get number of startup failures, function call failed for project %s", projname);
				exit(EXIT_FAILURE);
			    }
			    else if (max_nr_process_crashes > retval+1)
			    {
				process_manager_start_new_process_detached(1, projname, 0);
			    }
			    else
			    {
				printlog("WARNING: max number (%d) of startup failures in project %s reached."
					" Stoppped creating new processes until the configuration for this project has changed",
					max_nr_process_crashes, projname);
			    }
			    free(projname);
			}
			else
			{
			    printlog("WARNING: no project found for pid %d", pid);
			}
		    }
		}

		/* change state of the process to STATE_EXIT
		 * and move the entry to the shutdown list
		 */
		process_manager_cleanup_process(pid);
		qgis_shutdown_add_process(pid);
	    }
	    else
	    {
		logerror("ERROR: kill(%d,0) returned", pid);
		exit(EXIT_FAILURE);
	    }
	}
	else
	{
	    // process still exists, do not akt on it
	}
    }

    db_free_list_process(pidlist, listlen);
}


/* like process_manager_process_died() but without intensive tests */
void process_manager_process_died_during_init(pid_t pid, const char *projname)
{
    int retval = get_program_shutdown();
    if (!retval)
    {
	printlog("WARNING: project %s process %d died during init", projname, pid);
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
	printlog("ERROR: can not get socket fd from process %d during cleanup", pid);
    else
	close(fd);
    db_process_set_state_exit(pid);
}


