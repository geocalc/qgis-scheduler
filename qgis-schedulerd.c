/*
    Scheduler program for QGis server program. Talks FCGI with web server
    and FCGI with QGis server. Selects the right QGIS server program based on
    the URL given from web-gis client program.

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

/* Definition of process status
 *
 * A process is PROC_BUSY if it calculates a request.
 *
 * A process is PROC_OPEN_IDLE if it does not calculate a request and keeps the
 *  previous used connection open. This should not happen during the execution
 *  of this program.
 *
 * A process is PROC_IDLE if it waits for a connection on the socket or network
 *  file descriptor.
 *
 * A process is PROC_INIT if it is fed with project data for the first time
 *  after its start.
 *
 * The state diagram show these transitions:
 * START->PROC_INIT
 * PROC_INIT->PROC_IDLE	(ready to accept socket connection)
 * PROC_IDLE->PROC_BUSY (calculating a request)
 * PROC_BUSY->PROC_IDLE	(ready to accept socket connection)
 * any state->TERMINATE/END
 */

/* Description of dis-/connect behavior
 *
 * A network connection request arrives. The request is accept()ed.
 * A new thread is spawned and gets the file descriptor of the accepted network
 * connection.
 * The thread reads the input from network until the http URL arrives. We parse
 * the URL for the name of the QGIS project.
 * With the QGIS project a suitable process list is selected. In this we have
 * a list of child processes working this project.
 * Each entry in this process list got the connection data (client-server
 * socket file descriptor, pid, busy state).
 * We select an idle process and connect to it. If the number of idle processes
 * drops below a threshold, a new process is started, fed with
 * project data (state init) and set idle (state idle).
 *
 * To connect to a process we create a new thread, hand over the network file
 * descriptor and the handle to the process entry.
 * The thread writes its id to the process entry and connects to the socket to
 * the child process.
 * If the connection succeeds the thread marks the busy state of the child
 * process to be busy (state busy).
 *
 * If the network connection closes the thread ends.
 * If the socket connection closes the thread ends.
 * If the data from socket or network sends the FCGI token FCGI_END_REQUEST
 * the thread ends.
 * If the data from network sends the FCGI token FCGI_ABORT_REQUEST the thread
 * ends.
 *
 * If the thread ends, the thread id is removed from the process entry
 * handling the project, the process state is set to idle, the network
 * connection is closed as well as the socket connection.
 *
 * If too many idle processes are lingering around some of them may be closed,
 * so this programs sends a TERM or KILL signal to the child process.
 *
 * If a child process closes itself, a new process is spawned, fed with
 * project data and set idle.
 */

/* Behavior during many connection requests:
 *
 * An fcgi connection request arrives via ip network.
 * All processes (for the given project) are IN_USE.
 * Start a new process, connect to it, feed it the connection data from
 * network and deliver its result.
 *
 * An fcgi connection request arrives via ip network.
 * A process (for the given project) is OPEN_IDLE.
 * Close the current connection to the process and open a new connection to it.
 * Note: Another approach may be to reuse the existing connection to the
 * process by giving its corresponding worker thread the new network connection.
 */



#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <fastcgi.h>

#include "qgis_process.h"
#include "qgis_process_list.h"
#include "fcgi_state.h"

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })
#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })


//#define PRINT_NETWORK_DATA
//#define PRINT_SOCKET_DATA


struct thread_init_new_child_args
{
    struct qgis_process_s *proc;
};

struct thread_connection_handler_args
{
    int new_accepted_inet_fd;
};

//struct thread_start_new_child_args
//{
//    int id;
//    struct qgis_process_s *proc;
//};


static const char base_socket_desc[] = "qgis-schedulerd-socket";
static const int default_max_transfer_buffer_size = 4*1024; //INT_MAX;
static const int default_min_free_processes = 5;


#ifndef _GNU_SOURCE
const char *basename(const char *path)
{
    const char *base = strrchr(path, '/');

    if ('/' == *base)
    {
	return base + 1;
    }

    return base;
}
#endif


void usage(const char *argv0)
{
    //fprintf(stdout, "usage: %s [-h] [-d] [-c <CONFIGFILE>]\n", basename(argv0));
    fprintf(stdout, "usage: %s [-h] [-d] <command>\n", basename(argv0));
    fprintf(stdout, "\t-h: print this help\n");
    fprintf(stdout, "\t-d: do NOT become daemon\n");
    //fprintf(stdout, "\t-c: use CONFIGFILE (default '%s')\n", DEFAULT_CONFIG_PATH);
}




/* This global number is counted upwards, overflow included.
 * Every new unix socket gets this number attached creating a unique socket
 * path. In the very unlikely case the number is already given to an existing
 * socket path, the number is counted upwards again until we find a path which
 * is not assigned to a socket.
 * I assume here we do not create UINT_MAX number of sockets in this computer..
 */
static unsigned int socket_id = 0;
pthread_mutex_t socket_id_mutex = PTHREAD_MUTEX_INITIALIZER;


//static const int nr_childs = 2;
#define nr_childs	2
const char *command = NULL;
struct qgis_process_s *childprocs[nr_childs];
struct qgis_process_list_s *proclist = NULL;

void *thread_init_new_child(void *arg)
{
    assert(arg);
    struct thread_init_new_child_args *tinfo = arg;
    struct qgis_process_s *childproc = tinfo->proc;
    assert(childproc);
    const pthread_t thread_id = pthread_self();

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

    fprintf(stderr, "init new spawned child process\n");

    if (childproc)
    {
	qgis_process_set_state_idle(childproc);
    }
    else
    {
	fprintf(stderr, "no child process found to initialize\n");
	exit(EXIT_FAILURE);
    }
    free(arg);

    return NULL;
}


void *thread_start_new_child(void *arg)
{
    fprintf(stderr, "start new child process\n");
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
    pthread_mutex_lock (&socket_id_mutex);
    unsigned int socket_suffix_start = socket_id-1;
    pthread_mutex_unlock (&socket_id_mutex);

    for (;;)
    {
	pthread_mutex_lock (&socket_id_mutex);
	unsigned int socket_suffix = socket_id++;
	pthread_mutex_unlock (&socket_id_mutex);

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

	retval = bind(childsocket, &childsockaddr, sizeof(childsockaddr));
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

	/* close file descriptor stdin = 0
	 * assign socket file descriptor to fd 0
	 * fork
	 * exec
	 */
	int ret = dup2(childsocket, FCGI_LISTENSOCK_FILENO);
	if (-1 == ret)
	{
	    perror("error calling dup2");
	    exit(EXIT_FAILURE);
	}
//	const char *command = iniparser_getstring(ini, CGI_PATH_KEY,
//		CGI_PATH_KEY_DEFAULT);


	/* close all file descriptors different from 0. The fd different from
	 * 1 and 2 are opened during open() and socket() calls with FD_CLOEXEC
	 * flag enabled. This way, the fds are closed during exec() call.
	 * TODO: assign an error log file to fd 1 and 2
	 */
	close(1);
	close(2);


	execl(command, command, NULL);
	fprintf(stderr, "could not execute '%s': ", command);
	perror(NULL);
	exit(EXIT_FAILURE);
    }
    else if (0 < pid)
    {
	/* parent */
	struct qgis_process_s *childproc = qgis_process_new(pid, childsocket);
	qgis_process_list_add_process(proclist, childproc);

	/* NOTE: aside from the general rule
	 * "malloc() and free() within the same function"
	 * we transfer the responsibility for this memory
	 * to the thread itself.
	 */
	struct thread_init_new_child_args *targs = malloc(sizeof(*targs));
	targs->proc = childproc;
	pthread_t thread;
	pthread_create(&thread, NULL, thread_init_new_child, targs);

    }
    else
    {
	/* error */
	perror("can not fork");
	exit(EXIT_FAILURE);
    }


    return NULL;
}


void start_new_process_detached(int num)
{
    /* NOTE: aside from the general rule
     * "malloc() and free() within the same function"
     * we transfer the responsibility for this info memory
     * to the thread itself.
     */
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
	retval = pthread_create(&thread, &thread_attr, thread_start_new_child, NULL);
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


void *thread_handle_connection(void *arg)
{
    /* the main thread has been notified about data on the server network fd.
     * it accept()ed the new connection and passes the new file descriptor to
     * this thread. here we find an idling child process,  connect() to that
     * process and transfer the data between the network fd and the child
     * process fd and back.
     */
    assert(arg);
    struct thread_connection_handler_args *tinfo = arg;
    int inetsocketfd = tinfo->new_accepted_inet_fd;
    struct qgis_process_s *proc = NULL;
    struct sockaddr_un sockaddr;
    socklen_t sockaddrlen = sizeof(sockaddr);
    const pthread_t thread_id = pthread_self();

    /* detach myself from the main thread. Doing this to collect resources after
     * this thread ends. Because there is no join() waiting for this thread.
     */
    int retval = pthread_detach(thread_id);
    if (-1 == retval)
    {
	perror("error detaching thread");
	exit(EXIT_FAILURE);
    }

    fprintf(stderr, "start a new connection thread\n");

    /* find the next idling process and attach a thread to it */
    proc = qgis_process_list_mutex_find_process_by_status(proclist, PROC_IDLE);
    if ( !proc )
    {
	/* Found no idle processes.
	 * What now?
	 * All busy, close the network connection.
	 * Sorry guys.
	 */
	fprintf(stderr, "found no free process for network request. close connection\n");
	// NOTE: no mutex unlock here. we tried all processes, locked and unlocked them all

	/* get the number of new started processes which then become idle
	 * processes.
	 * Then we can estimate how much new processes need to be started.
	 */
	/* NOTE: between the call to qgis_process_list_mutex_find_process_by_status()
	 * and the last call to qgis_process_list_get_num_process_by_status()
	 * the numbers may change (significant). Do we need a separate lock?
	 * I think we do not. Because:
	 * If the result number is too low we start too much new processes. But
	 * these processes get killed some time afterwards if we don't need
	 * them.
	 * If the result number is too high we got not enough processes. But
	 * this would mean the number of proc_state_start and proc_state_init
	 * is too high, which is not the case. Because the processes transit
	 * from PROC_START over PROC_INIT to PROC_IDLE. So if we have the
	 * correct numbers of proc_state_start and proc_state_init during
	 * a moment all these processes certainly become PROC_IDLE. If we
	 * read_lock the process list during counting the state, there is
	 * no count error (in transition from PROC_START to PROC_INIT).
	 * We get the correct number if we first count PROC_INIT and then
	 * PROC_START, not the other way around.
	 */
	int proc_state_init = qgis_process_list_get_num_process_by_status(proclist, PROC_INIT);
	int proc_state_start = qgis_process_list_get_num_process_by_status(proclist, PROC_START);

	int missing_processes = default_min_free_processes - (proc_state_init + proc_state_start);
	if (missing_processes > 0)
	{
	    /* not enough free processes, start new ones */
	    start_new_process_detached(missing_processes);
	}
    }
    else
    {
	qgis_process_set_state_busy(proc, thread_id);
	pthread_mutex_t *mutex = qgis_process_get_mutex(proc);
	pthread_mutex_unlock(mutex);
	mutex = NULL;

	int childunixsocketfd;

	/* get the address of the socket transferred to the child process,
	 * then connect to it.
	 */
	childunixsocketfd = qgis_process_get_socketfd(proc);	// refers to the socket the child process accept()s from
	retval = getsockname(childunixsocketfd, &sockaddr, &sockaddrlen);
	if (-1 == retval)
	{
	    perror("error retrieving the name of child process socket");
	    exit(EXIT_FAILURE);
	}
	/* leave the original child socket and create a new one on the opposite
	 * side.
	 */
	retval = socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
	if (-1 == retval)
	{
	    perror("error: can not create socket to child process");
	    exit(EXIT_FAILURE);
	}
	childunixsocketfd = retval;	// refers to the socket this program connects to the child process
	retval = connect(childunixsocketfd, &sockaddr, sizeof(sockaddr));
	if (-1 == retval)
	{
	    perror("error: can not connect to child process");
	    exit(EXIT_FAILURE);
	}


	/* get the maximum read write socket buffer size */
	int maxbufsize = default_max_transfer_buffer_size;
	{
	    int sockbufsize = 0;
	    socklen_t size = sizeof(sockbufsize);
	    retval = getsockopt(childunixsocketfd, SOL_SOCKET, SO_SNDBUF, &sockbufsize, &size);
	    if (-1 == retval)
	    {
		perror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);

	    size = sizeof(sockbufsize);
	    retval = getsockopt(childunixsocketfd, SOL_SOCKET, SO_RCVBUF, &sockbufsize, &size);
	    if (-1 == retval)
	    {
		perror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);

	    size = sizeof(sockbufsize);
	    retval = getsockopt(inetsocketfd, SOL_SOCKET, SO_SNDBUF, &sockbufsize, &size);
	    if (-1 == retval)
	    {
		perror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);

	    size = sizeof(sockbufsize);
	    retval = getsockopt(inetsocketfd, SOL_SOCKET, SO_RCVBUF, &sockbufsize, &size);
	    if (-1 == retval)
	    {
		perror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);

	    fprintf(stderr, "set maximum transfer buffer to %d\n", maxbufsize);

	}
	char *buffer = malloc(maxbufsize);
	assert(buffer);

	int session_start = 1;

	int maxfd = 0;
	fd_set rfds;
	fd_set wfds;
	int can_read_networksock = 0;
	int can_write_networksock = 0;
	int can_read_unixsock = 0;
	int can_write_unixsock = 0;


	if (inetsocketfd > maxfd)
	    maxfd = inetsocketfd;
	if (childunixsocketfd > maxfd)
	    maxfd = childunixsocketfd;

	maxfd++;

	/* set timeout to infinite */
	//    struct timeval timeout;
	//    timeout.tv_sec = 60;	// wait 60 seconds for a child process to communicate
	//    timeout.tv_usec = 0;

	int has_finished = 0;
	while ( !has_finished )
	{
	    /* wait for connections, signals or timeout */
	    //	timeout.tv_sec = 60;	// wait 60 seconds for a child process to communicate
	    //	timeout.tv_usec = 0;

	    fprintf(stderr, "[%ld] selecting on network connections\n", thread_id);
	    FD_ZERO(&rfds);
	    FD_ZERO(&wfds);
	    if ( !can_read_networksock )
		FD_SET(inetsocketfd, &rfds);
	    if ( !can_write_networksock )
		FD_SET(inetsocketfd, &wfds);
	    if ( !can_read_unixsock )
		FD_SET(childunixsocketfd, &rfds);
	    if ( !can_write_unixsock )
		FD_SET(childunixsocketfd, &wfds);
	    retval = select(maxfd, &rfds, &wfds, NULL, NULL /*&timeout*/);
	    if (-1 == retval)
	    {
		switch (errno)
		{
		case EINTR:
		    /* We received a termination signal.
		     * End this thread, close all file descriptors
		     * and let the main thread clean up.
		     */
		    fprintf(stderr, "thread_handle_connection() received interrupt\n");
		    break;

		default:
		    perror("error: thread_handle_connection() calling select");
		    exit(EXIT_FAILURE);
		    // no break needed
		}
		break;
	    }

	    if (FD_ISSET(inetsocketfd, &wfds))
	    {
		fprintf(stderr, "[%ld]  can write to network socket\n", thread_id);
		can_write_networksock = 1;
	    }
	    if (FD_ISSET(childunixsocketfd, &wfds))
	    {
		fprintf(stderr, "[%ld]  can write to unix socket\n", thread_id);
		can_write_unixsock = 1;
	    }
	    if (FD_ISSET(inetsocketfd, &rfds))
	    {
		fprintf(stderr, "[%ld]  can read from network socket\n", thread_id);
		can_read_networksock = 1;
	    }
	    if (FD_ISSET(childunixsocketfd, &rfds))
	    {
		fprintf(stderr, "[%ld]  can read from unix socket\n", thread_id);
		can_read_unixsock = 1;
	    }

	    if (can_read_networksock && can_write_unixsock)
	    {
		fprintf(stderr, "[%ld]  read data from network socket: ", thread_id);
		int readbytes = read(inetsocketfd, buffer, maxbufsize);
		fprintf(stderr, "read %d, ", readbytes);
		if (-1 == readbytes)
		{
		    perror("\nerror: reading from network socket");
		    exit(EXIT_FAILURE);
		}
		else if (0 == readbytes)
		{
		    /* end of file received. exit this thread */
		    break;
		}
#ifdef PRINT_NETWORK_DATA
		fprintf(stderr, "\n[%ld] network data:\n", thread_id);
		fwrite(buffer, 1, readbytes, stderr);
		fprintf(stderr, "\n");
#endif
		if (session_start)
		{
		    /* check the status of this fcgi session
		     * if this session starts
		     * and got a flag to keep this session open
		     * then delete that flag
		     * and pass the deleted flag message to the child process */
		    /* TODO: this is slightly incorrect. what if the message is
		     * incomplete? dont we need a better fcgi session management?
		     */
		    struct fcgi_message_s *message = fcgi_state_new_message();
		    retval = fcgi_state_parse_message(message, buffer, readbytes);
		    if (FCGI_BEGIN_REQUEST == fcgi_state_get_message_type(message))
		    {
			if (FCGI_RESPONDER == fcgi_state_get_message_role(message))
			{
			    int flag =  fcgi_state_get_message_flag(message);
			    if (flag >= 0)
			    {
				if (flag & FCGI_KEEP_CONN)
				{
				    flag &= ~FCGI_KEEP_CONN; // delete connection keep flag.
				    fcgi_state_set_message_flag(message, flag);
				    fcgi_state_message_write((unsigned char *)buffer, readbytes, message);
				}
			    }
			}
		    }
		    fcgi_state_delete_message(message);
		    session_start = 0;
		}
		int writebytes = write(childunixsocketfd, buffer, readbytes);
		fprintf(stderr, "[%ld] wrote %d\n", thread_id, writebytes);
		if (-1 == writebytes)
		{
		    perror("error: writing to child process socket");
		    exit(EXIT_FAILURE);
		}
		can_read_networksock = 0;
		can_write_unixsock = 0;
	    }

	    if (can_read_unixsock && can_write_networksock)
	    {
		fprintf(stderr, "[%ld]  read data from unix socket: ", thread_id);
		int readbytes = read(childunixsocketfd, buffer, maxbufsize);
		fprintf(stderr, "read %d, ", readbytes);
		if (-1 == readbytes)
		{
		    perror("\nerror: reading from child process socket");
		    exit(EXIT_FAILURE);
		}
		else if (0 == readbytes)
		{
		    /* end of file received. exit this thread */
		    break;
		}
#ifdef PRINT_SOCKET_DATA
		fprintf(stderr, "fcgi data:\n");
		fwrite(buffer, 1, readbytes, stderr);
		fprintf(stderr, "\n");
#endif
		int writebytes = write(inetsocketfd, buffer, readbytes);
		fprintf(stderr, "wrote %d\n", writebytes);
		if (-1 == writebytes)
		{
		    perror("error: writing to network socket");
		    exit(EXIT_FAILURE);
		}
//		{
//		    /* check the status of this fcgi session */
//		    struct fcgi_message_s *message = fcgi_state_new_message();
//		    retval = fcgi_state_parse_message(message, buffer, readbytes);
//		    if (fcgi_state_get_message_parse_done(message))
//		    {
//			if (FCGI_END_REQUEST == fcgi_state_get_message_type(message))
//			{
//			    fprintf(stderr, "fcgi session end. close connection\n");
//			    fcgi_state_delete_message(message);
//			    break;
//			}
//		    }
//		    fcgi_state_delete_message(message);
//		}
		can_read_unixsock = 0;
		can_write_networksock = 0;
	    }

	}
	close (childunixsocketfd);
	free(buffer);
	qgis_process_set_state_idle(proc);
    }

    /* clean up */
    fprintf(stderr, "[%ld] end connection thread\n", thread_id);
    close (inetsocketfd);
    free(arg);

    return NULL;
}


int do_terminate = 0;	// in process to terminate itself (=1) or not (=0)

/* act on signals */
/* TODO: sometimes the program hangs during shutdown.
 *       we receive a signal (SIGCHLD) but dont react on it?
 */
void signalaction(int signal, siginfo_t *info, void *ucontext)
{
    fprintf(stderr, "got signal %d from pid %d\n", signal, info->si_pid);
    switch (signal)
    {
    case SIGCHLD:
    {
	/* get pid of terminated child process */
	pid_t pid = info->si_pid;
	/* get array id of terminated child process */
	struct qgis_process_s *proc = qgis_process_list_find_process_by_pid(proclist, pid);
	if ( !proc )
	{
	    /* pid does not belong to our child processes ? */
	    fprintf(stderr, "pid %d does not belong to us?\n", pid);
	}
	else
	{
	    fprintf(stderr, "process %d ended\n", pid);
	    /* Erase the old entry. The process does not exist anymore */
	    qgis_process_list_remove_process(proclist, proc);
	    qgis_process_delete(proc);

	    if ( !do_terminate )
	    {
		fprintf(stderr, "restarting process\n");

		/* child process terminated, restart anew */
		/* TODO: react on child processes exiting immediately.
		 * maybe store the creation time and calculate the execution time?
		 */
		start_new_process_detached(1);
	    }
	}
	break;
    }
    case SIGTERM:	// fall through
    case SIGINT:
    case SIGQUIT:
	/* termination signal, kill all child processes */
	fprintf(stderr, "exit program\n");
	do_terminate = 1;
	break;
    }
}


int main(int argc, char **argv)
{
    int exitvalue = EXIT_SUCCESS;
    const int port = 10177;
    int no_daemon = 0;
    int serversocketfd = -1;
    memset(childprocs, 0, sizeof(childprocs));

    int opt;

    while ((opt = getopt(argc, argv, "hd")) != -1)
    {
	switch (opt)
	{
	case 'h':
	    usage(argv[0]);
	    return EXIT_SUCCESS;
	case 'd':
	    no_daemon = 1;
	    break;
//	case 'c':
//	    config_path = optarg;
//	    break;
	default: /* '?' */
	    usage(argv[0]);
	    return EXIT_FAILURE;
	}
    }

    if (optind >= argc)
    {
	printf("error: missing command\n");
	usage(argv[0]);
	return EXIT_FAILURE;
    }
    command = argv[optind++];

    /* prepare inet socket connection for application server process (this)
     */

    {
	struct addrinfo hints;
	struct addrinfo *result = NULL, *rp = NULL;
	const int port_len = 10;
	char str_port[port_len];
	snprintf(str_port,port_len,"%d",port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC; /* Allow IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM; /* TCP socket */
	hints.ai_flags = AI_PASSIVE; /* For wildcard IP address */
	//hints.ai_protocol = 0;          /* Any protocol */
	//hints.ai_canonname = NULL;
	//hints.ai_addr = NULL;
	//hints.ai_next = NULL;

	int s = getaddrinfo(NULL, str_port, &hints, &result);
	if (s != 0)
	{
	    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
	    exit(EXIT_FAILURE);
	}

	/* getaddrinfo() returns a list of address structures.
	 Try each address until we successfully bind(2).
	 If socket(2) (or bind(2)) fails, we (close the socket
	 and) try the next address. */
	for (rp = result; rp != NULL; rp = rp->ai_next)
	{
	    //printf("try family %d, socket type %d, protocol %d\n",rp->ai_family,rp->ai_socktype,rp->ai_protocol);
	    serversocketfd = socket(rp->ai_family, rp->ai_socktype|SOCK_NONBLOCK|SOCK_CLOEXEC, rp->ai_protocol);
	    if (serversocketfd == -1)
	    {
		//printf(" could not create socket\n");
		perror(" could not create socket for network data");
		continue;
	    }

	    if (bind(serversocketfd, rp->ai_addr, rp->ai_addrlen) == 0)
		break; /* Success */

	    //printf(" could not bind to socket\n");
	    perror(" could not bind to network socket");
	    close(serversocketfd);
	}

	if (rp == NULL)
	{ /* No address succeeded */
	    //fprintf(stderr, "Could not bind\n"); // TODO better message
	    perror("could not create network socket");
	    exit(EXIT_FAILURE);
	}

	freeaddrinfo(result); /* No longer needed */
    }


    /* we are server. listen to incoming connections */
    int retval = listen(serversocketfd, SOMAXCONN);
    if (retval)
    {
	perror("error: can not listen to socket");
	exit(EXIT_FAILURE);
    }



    if ( !no_daemon )
    {
	const int no_change_dir = 0;
	const int no_close_streams = 1;
	retval = daemon(no_change_dir,no_close_streams);
	if (retval)
	{
	    perror("error: can not become daemon");
	    exit(EXIT_FAILURE);
	}
    }


    /* prepare the signal reception.
     * This way we can start a new child if one has exited on its own,
     * or we can kill the children if this management process got signal
     * to terminate.
     */
    struct sigaction action;
    action.sa_sigaction = signalaction;
    action.sa_flags = SA_SIGINFO|SA_NOCLDSTOP|SA_NOCLDWAIT;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGCHLD);
    sigaddset(&action.sa_mask, SIGTERM);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGQUIT);
    retval = sigaction(SIGTERM, &action, NULL);
    if (retval)
    {
	perror("error: can not install signal handler");
	exit(EXIT_FAILURE);
    }
    retval = sigaction(SIGQUIT, &action, NULL);
    if (retval)
    {
	perror("error: can not install signal handler");
	exit(EXIT_FAILURE);
    }
    retval = sigaction(SIGCHLD, &action, NULL);
    if (retval)
    {
	perror("error: can not install signal handler");
	exit(EXIT_FAILURE);
    }
    retval = sigaction(SIGINT, &action, NULL);
    if (retval)
    {
	perror("error: can not install signal handler");
	exit(EXIT_FAILURE);
    }


    /* start the children */
    proclist = qgis_process_list_new();
    pthread_t threads[nr_childs];
    int i;
    for (i=0; i<nr_childs; i++)
    {
	pthread_create(&threads[i], NULL, thread_start_new_child, NULL);
    }
    for (i=0; i<nr_childs; i++)
	pthread_join(threads[i], NULL);


    /* wait for signals of child processes exiting (SIGCHLD) or to terminate
     * this program (SIGTERM, SIGINT) or clients connecting via network to
     * this server.
     */

    fd_set rfds;

    /* set timeout to infinite */
    struct timeval timeout, *timeout_ptr = NULL;
    timeout.tv_sec = 10;	// wait 10 seconds for a child process to terminate
    timeout.tv_usec = 0;

    int has_finished_first_run = 0;
    int has_finished_second_run = 0;
    int has_finished = 0;
    while ( !has_finished )
    {
	/* wait for connections, signals or timeout */
	/* NOTE: I expect a linux behavior over here:
	 * If select() is interrupted by a signal handler, the timeout value
	 * is modified to contain the remaining time.
	 */
	FD_ZERO(&rfds);
	FD_SET(serversocketfd, &rfds);
	retval = select(serversocketfd+1, &rfds,NULL,NULL,timeout_ptr);
	if (-1 == retval)
	{
	    switch (errno)
	    {
	    case EINTR:
		/* We received an interrupt, possibly a termination signal.
		 * Let the main thread clean up: Wait for all child processes
		 * to end, close all remaining file descriptors and exit.
		 */
		fprintf(stderr, "main() received interrupt\n");
		break;

	    default:
		perror("error: main() calling select");
		exit(EXIT_FAILURE);
		// no break needed
	    }
	}

	/* over here I expect the main thread to continue AFTER the signal
	 * handler has ended its thread.
	 * If this expectation does not fulfill we have to look for a different
	 * design in this section.
	 */
	if ( do_terminate )
	{
	    /* On the first run send all child processes the TERM signal,
	     * then wait for the processes to exit normally. During this
	     * we are called by the signal handler for the CHLD signal.
	     * During this we check for all child processes to terminate.
	     * If all processed have terminated, we do exit this. Else we start
	     * a second round of killing all processes and then exit this.
	     */

	    if (has_finished_second_run)
	    {
		struct qgis_process_s *proc = qgis_process_list_get_first_process(proclist);

		if ( !proc )
		{
		    /* all child processes did exit, we can end this */
		    exitvalue = EXIT_SUCCESS;
		    break;
		}

		/* if none of the child processes did terminate on timeout
		 * we exit with failure
		 */
		if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
		{
		    exitvalue = EXIT_FAILURE;
		    break;
		}
		// else restart the sleep() with remaining timeout
	    }
	    else if (has_finished_first_run)
	    {
		struct qgis_process_s *proc = qgis_process_list_get_first_process(proclist);
		if ( !proc )
		{
		    /* all child processes did exit, we can end this */
		    exitvalue = EXIT_SUCCESS;
		    break;
		}

		/* if none of the child processes did terminate on timeout
		 * we can start the next round of sending signals
		 */
		if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
		{
		    int children = 0;
		    pid_t *pidlist = NULL;
		    int pidlen = 0;
		    retval = qgis_process_list_get_pid_list(proclist, &pidlist, &pidlen);
		    for(i=0; i<pidlen; i++)
		    {
			pid_t pid = pidlist[i];
			retval = kill(pid, SIGKILL);
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
				perror("error: could not send KILL signal");
			    }
			}
			else
			{
			    children++;
			}

		    }
		    free(pidlist);
		    if (0 < children)
		    {
			fprintf(stderr, "termination signal received, sending SIGKILL to %d child processes..\n", children);
			timeout.tv_sec = 10;
			has_finished_second_run = 1;
		    }
		    else
		    {
			/* no more child processes found to send signal.
			 * exit immediately.
			 */
			fprintf(stderr, "termination signal received, shut down\n");
			break;
		    }

		}
		// else restart the sleep() with remaining timeout

	    }
	    else
	    {
		int children = 0;
		pid_t *pidlist = NULL;
		int pidlen = 0;
		retval = qgis_process_list_get_pid_list(proclist, &pidlist, &pidlen);
		for(i=0; i<pidlen; i++)
		{
		    pid_t pid = pidlist[i];
		    retval = kill(pid, SIGTERM);
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

		if (0 < children)
		{
		    fprintf(stderr, "termination signal received, sending SIGTERM to %d child processes..\n", children);
		    /* set timeout to "some seconds" */
		    timeout_ptr = &timeout;
		    timeout.tv_sec = 10;
		    has_finished_first_run = 1;
		}
		else
		{
		    /* no more child processes found to send signal.
		     * exit immediately.
		     */
		    fprintf(stderr, "termination signal received, no more processes to signal, shut down\n");
		    break;
		}
	    }

	}
	else if (retval)
	{
	    /* connection available */
	    if (FD_ISSET(serversocketfd, &rfds))
	    {
		struct sockaddr addr;
		socklen_t addrlen = sizeof(addr);
		retval = accept(serversocketfd, &addr, &addrlen);
		if (-1 == retval)
		{
		    perror("error: calling accept");
		    exit(EXIT_FAILURE);
		}
		{
		    char hbuf[80], sbuf[10];
		    int ret = getnameinfo(&addr, addrlen, hbuf, sizeof(hbuf), sbuf,
	                       sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV);
		    if (ret < 0)
		    {
			fprintf(stderr, "error: can not convert host address: %s\n", gai_strerror(ret));
		    }
		    else
		    {
			fprintf(stderr, "accepted connection from host %s, port %s, fd %d\n", hbuf, sbuf, retval);
		    }
		}
		int networkfd = retval;

		{
		    /* NOTE: aside from the general rule
		     * "malloc() and free() within the same function"
		     * we transfer the responsibility for this memory
		     * to the thread itself.
		     */
		    struct thread_connection_handler_args *targs = malloc(sizeof(*targs));
		    targs->new_accepted_inet_fd = networkfd;
		    pthread_t thread;
		    pthread_create(&thread, NULL, thread_handle_connection, targs);
		}
	    }
	}
	else
	{
	    /* no further data available */
	}

    }

    fprintf(stderr, "closing network socket\n");
    fflush(stderr);
    close(serversocketfd);
    qgis_process_list_delete(proclist);

    return exitvalue;
}
