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
#include <fastcgi.h>


struct thread_info
{
    int new_server_fd;
};


static const char base_socket_desc[] = "qgis-schedulerd-socket";

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

enum process_state_e
{
    MY_PROC_START = 0,
    MY_PROC_INIT,
    MY_PROC_IDLE,
    MY_PROC_OPEN_IDLE,
    MY_PROC_BUSY,
};

//static const int nr_childs = 2;
#define nr_childs	1
pid_t child_pid[nr_childs];
const char *command = NULL;
int childsocket[nr_childs] = {-1};
enum process_state_e status[nr_childs] = {MY_PROC_START};

pid_t start_new_child(int arraynr)
{
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
    childsocket[arraynr] = retval;

    for (;;)
    {
	int socket_suffix = socket_id++;
	struct sockaddr_un childsockaddr;
	childsockaddr.sun_family = AF_UNIX;
	retval = snprintf( childsockaddr.sun_path, sizeof(childsockaddr.sun_path), "%c%s%u", '\0', base_socket_desc, socket_suffix );
	if (-1 == retval)
	{
	    perror("error calling string format function snprintf");
	    exit(EXIT_FAILURE);
	}

	retval = bind(childsocket[arraynr], &childsockaddr, sizeof(childsockaddr));
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
    retval = listen(childsocket[arraynr], 1);
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
	assert(childsocket[arraynr] != FCGI_LISTENSOCK_FILENO);
	int ret = dup2(childsocket[arraynr], FCGI_LISTENSOCK_FILENO);
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
	child_pid[arraynr] = pid;
	status[arraynr] = MY_PROC_IDLE;
    }
    else
    {
	/* error */
	perror("can not fork");
	exit(EXIT_FAILURE);
    }

    return pid;
}

void *thread_handle_connection(void *arg)
{
    /* the main thread has been notified about data on the server network fd.
     * it accept()ed the new connection and passes the new file descriptor to
     * this thread. here we connect() to the child thread and transfer the data
     * between the network fd and the child process fd and back.
     */
	/* TODO: get the network packet size of this connection and
	 * adopt the packet transfer size to the network packet size.
	 */
    assert(arg);
    struct thread_info *tinfo = arg;
    int serversocketfd = tinfo->new_server_fd;
    struct sockaddr_un sockaddr;
    socklen_t sockaddrlen = sizeof(sockaddr);
    int childfd;
    static const int buffersize = 1024;
    char buffer[buffersize];
    const pthread_t thread_id = pthread_self();
    int free_socket;



    fprintf(stderr, "starting new connection thread\n");
    for (free_socket = 0; free_socket<nr_childs; free_socket++)
    {
	if (MY_PROC_IDLE == status[free_socket])
	    break;
    }
    if (nr_childs == free_socket)
    {
	/* all child processes busy */
	fprintf(stderr, "error: all %d child programs are busy. disconnect\n", nr_childs);
	close(serversocketfd);
	return NULL;
    }


    /* get the address of the socket transferred to the child process,
     * then connect to it.
     */
    int retval = getsockname(childsocket[free_socket], &sockaddr, &sockaddrlen);
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
    childfd = retval;
    retval = connect(childfd, &sockaddr, sizeof(sockaddr));
    if (-1 == retval)
    {
	perror("error: can not connect to child process");
	exit(EXIT_FAILURE);
    }


    int maxfd = 0;
    fd_set rfds;
    fd_set wfds;
    int can_read_networksock = 0;
    int can_write_networksock = 0;
    int can_read_unixsock = 0;
    int can_write_unixsock = 0;


    if (serversocketfd > maxfd)
	maxfd = serversocketfd;
    if (childfd > maxfd)
	maxfd = childfd;

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
	    FD_SET(serversocketfd, &rfds);
	if ( !can_write_networksock )
	    FD_SET(serversocketfd, &wfds);
	if ( !can_read_unixsock )
	    FD_SET(childfd, &rfds);
	if ( !can_write_unixsock )
	    FD_SET(childfd, &wfds);
	retval = select(maxfd, &rfds, &wfds, NULL, NULL /*&timeout*/);
	if (-1 == retval)
	{
	    perror("error: thread_handle_connection() calling select");
	    exit(EXIT_FAILURE);
	}

	if (FD_ISSET(serversocketfd, &wfds))
	{
	    fprintf(stderr, "[%ld]  can write to network socket\n", thread_id);
	    can_write_networksock = 1;
	}
	if (FD_ISSET(childfd, &wfds))
	{
	    fprintf(stderr, "[%ld]  can write to unix socket\n", thread_id);
	    can_write_unixsock = 1;
	}
	if (FD_ISSET(serversocketfd, &rfds))
	{
	    fprintf(stderr, "[%ld]  can read from network socket\n", thread_id);
	    can_read_networksock = 1;
	}
	if (FD_ISSET(childfd, &rfds))
	{
	    fprintf(stderr, "[%ld]  can read from unix socket\n", thread_id);
	    can_read_unixsock = 1;
	}

	if (can_read_networksock && can_write_unixsock)
	{
	    fprintf(stderr, "[%ld]  read data from network socket: ", thread_id);
	    retval = read(serversocketfd, buffer, buffersize);
	    fprintf(stderr, "read %d, ", retval);
	    if (-1 == retval)
	    {
		perror("\nerror: reading from network socket");
		exit(EXIT_FAILURE);
	    }
	    else if (0 == retval)
	    {
		/* end of file received. exit this thread */
		break;
	    }
	    fprintf(stderr, "\n[%ld] network data:\n", thread_id);
	    fwrite(buffer, 1, retval, stderr);
	    fprintf(stderr, "\n");
	    retval = write(childfd, buffer, retval);
	    fprintf(stderr, "[%ld] wrote %d\n", thread_id, retval);
	    if (-1 == retval)
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
	    retval = read(childfd, buffer, buffersize);
	    fprintf(stderr, "read %d, ", retval);
	    if (-1 == retval)
	    {
		perror("\nerror: reading from network socket");
		exit(EXIT_FAILURE);
	    }
	    else if (0 == retval)
	    {
		/* end of file received. exit this thread */
		break;
	    }
//	    fprintf(stderr, "fcgi data:\n");
//	    fwrite(buffer, 1, retval, stderr);
//	    fprintf(stderr, "\n");
	    retval = write(serversocketfd, buffer, retval);
	    fprintf(stderr, "wrote %d\n", retval);
	    if (-1 == retval)
	    {
		perror("error: writing to child process socket");
		exit(EXIT_FAILURE);
	    }
	    can_read_unixsock = 0;
	    can_write_networksock = 0;
	}

    }

    /* clean up */
    fprintf(stderr, "[%ld] end connection thread\n", thread_id);
    close (childfd);
    close (serversocketfd);
    free(arg);

    return NULL;
}

int do_terminate = 0;	// in process to terminate itself (=1) or not (=0)

/* act on signals */
void signalaction(int signal, siginfo_t *info, void *ucontext)
{
    fprintf(stderr, "got signal %d\n", signal);
    switch (signal)
    {
    case SIGCHLD:
    {
	/* get pid of terminated child process */
	pid_t pid = info->si_pid;
	/* get array id of terminated child process */
	int i;
	for (i=0; i<nr_childs; i++)
	{
	    if (pid == child_pid[i])
		break;
	}
	if (i >= nr_childs)
	{
	    /* pid does not belong to our child processes ? */
	    // TODO print log message
	}
	else
	{
	    if ( !do_terminate )
	    {
		/* child process terminated, restart anew */
		/* TODO: react on child processes exiting immediately.
		 * maybe store the creation time and calculate the execution time?
		 */
		start_new_child(i);
	    }
	    else
	    {
		/* mark child process as terminated */
		child_pid[i] = 0;
		fprintf(stderr, "process %d ended\n", pid);
	    }
	}
	break;
    }
    case SIGTERM:	// fall through
    case SIGQUIT:
	/* termination signal, kill all child processes */
	do_terminate = 1;
	break;
    }
}


int main(int argc, char **argv)
{
    const int port = 10177;
    int no_daemon = 0;
    int serversocketfd = -1;
    memset(child_pid, 0, sizeof(child_pid));

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


    /* start the children */
    int i;
    for (i=0; i<nr_childs; i++)
    {
	start_new_child(i);
    }


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
	    perror("error: main() calling select");
	    exit(EXIT_FAILURE);
	}

	/* over here I expect the main thread to continue AFTER the signal
	 * handler has ended its thread.
	 * If this expectation does not fulfill we have to look for a different
	 * design in this section.
	 */
	else if ( do_terminate )
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
		int i;
		for (i=0; i<nr_childs; i++)
		{
		    if ( 0 != child_pid[i] )
		    {
			break;
		    }
		    if (i >= nr_childs)
		    {
			/* all child processes did exit, we can end this */
			return EXIT_SUCCESS;
		    }
		}

		/* if none of the child processes did terminate on timeout
		 * we exit with failure
		 */
		if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
		{
		    return EXIT_FAILURE;
		}
		// else restart the sleep() with remaining timeout
	    }
	    else if (has_finished_first_run)
	    {
		int i;
		for (i=0; i<nr_childs; i++)
		{
		    if ( 0 != child_pid[i] )
		    {
			break;
		    }
		}
		if (i >= nr_childs)
		{
		    /* all child processes did exit, we can end this */
		    return EXIT_SUCCESS;
		}

		/* if none of the child processes did terminate on timeout
		 * we can start the next round of sending signals
		 */
		if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
		{
		    int children = 0;
		    for (i=0; i<nr_childs; i++)
		    {
			if ( 0 != child_pid[i] )
			{
			    kill(child_pid[i], SIGKILL);
			    children++;
			}
		    }
		    fprintf(stderr, "termination signal received, sending SIGTERM to %d child processes..\n", children);
		    timeout.tv_sec = 10;
		    has_finished_second_run = 1;

		}
		// else restart the sleep() with remaining timeout

	    }
	    else
	    {
		int children = 0;
		int i;
		for (i=0; i<nr_childs; i++)
		{
		    if ( 0 != child_pid[i] )
		    {
			kill(child_pid[i], SIGTERM);
			children++;
		    }
		}
		fprintf(stderr, "termination signal received, sending SIGTERM to %d child processes..\n", children);
		/* set timeout to "some seconds" */
		timeout_ptr = &timeout;
		timeout.tv_sec = 10;
		has_finished_first_run = 1;
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
		/* NOTE: aside from the general rule
		 * "malloc() and free() within the same function"
		 * we transfer the responsibility for this info memory
		 * to the thread itself.
		 */
		struct thread_info *ti = malloc(sizeof(*ti));
		ti->new_server_fd = retval;
		pthread_t thread;
		pthread_create(&thread, NULL, thread_handle_connection, ti);
	    }
	}
	else
	{
	    /* no further data available */
	}

    }
    /* wait some seconds to check if every child has exited.
     * else send sigkill signal.
     */
    return EXIT_SUCCESS;
}
