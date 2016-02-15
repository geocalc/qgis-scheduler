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
#include <sys/queue.h>
#include <fastcgi.h>
#include <regex.h>
#include <fcntl.h>
#include <pwd.h>

#include "qgis_project_list.h"
//#include "qgis_project.h"
//#include "qgis_process.h"
//#include "qgis_process_list.h"
#include "fcgi_state.h"
#include "fcgi_data.h"
#include "qgis_config.h"
#include "qgis_inotify.h"
#include "logger.h"
#include "timer.h"
#include "qgis_shutdown_queue.h"

#include "config.h"

//#include <sys/types.h>	// für open()
//#include <sys/stat.h>
//#include <fcntl.h>


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
#ifndef DEFAULT_CONFIG_PATH
# define DEFAULT_CONFIG_PATH	"/etc/qgis-scheduler/qgis-scheduler.conf"
#endif


struct thread_connection_handler_args
{
    int new_accepted_inet_fd;
    char *hostname;
};


static const char version[] = VERSION;
static const int default_max_transfer_buffer_size = 4*1024; //INT_MAX;
static const int default_min_free_processes = 1;
static const int daemon_no_change_dir = 0;
static const int daemon_no_close_streams = 1;



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
    fprintf(stdout, "usage: %s [-h] [-V] [-d] [-c <CONFIGFILE>]\n", basename(argv0));
    //fprintf(stdout, "usage: %s [-h]  [-V] [-d]\n", basename(argv0));
    fprintf(stdout, "\t-h: print this help\n");
    fprintf(stdout, "\t-V: print version\n");
    fprintf(stdout, "\t-d: do NOT become daemon\n");
    fprintf(stdout, "\t-c: use CONFIGFILE (default '%s')\n", DEFAULT_CONFIG_PATH);
}


void print_version()
{
    fprintf(stdout, "%s\n", version);
}


void write_pid_file(const char *path)
{
    FILE *f = fopen(path, "w");
    if (NULL == f)
    {
	logerror("can not open pidfile '%s': ", path);
	exit(EXIT_FAILURE);
    }

    pid_t pid = getpid();
    fprintf(f, "%d", pid);
    fclose(f);
}


void remove_pid_file(const char *path)
{
    int retval = unlink(path);
    if (-1 == retval)
    {
	logerror("can not remove pidfile '%s': ", path);
	// intentionally no exit() call
    }
}



struct qgis_project_list_s *projectlist = NULL;

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
    int requestId;
    int role;

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

    debug(1, "start a new connection thread\n");

    struct timespec ts;
    retval = qgis_timer_start(&ts);
    if (-1 == retval)
    {
	logerror("clock_gettime(%d,..)", get_valid_clock_id());
	exit(EXIT_FAILURE);
    }


//    char debugfile[128];
//    sprintf(debugfile, "/tmp/threadconnect.%lu.dump", thread_id);
//    int debugfd = open(debugfile, (O_WRONLY|O_CREAT|O_TRUNC), (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH));
//    if (-1 == debugfd)
//    {
//	debug(1, "error can not open file '%s': ", debugfile);
//	logerror(NULL);
//	exit(EXIT_FAILURE);
//    }


    /* 1) accept fcgi data from the webserver.
     * 2) parse fcgi data and extract the query string
     * 3) parse the query string and extract the project name
     * 4) select the process list with the project name
     * 5) get the next idle process from the process list or
     *    return "server busy"
     * 6) connect to the idle process
     * 7) send all received data from the web server
     * 8) send and receive all data between web serve and fcgi program
     */
    /* TODO: if the web server sends a FCGI_GET_VALUES request, we should
     *       send the results on our own. So we can control the settings
     *       FCGI_MAX_CONNS=UINT16_MAX, FCGI_MAX_REQS=1, FCGI_MPXS_CONNS=0
     */
    /* TODO: if the web server sends a second request with a different
     *       requestId, we should immediately send FCGI_END_REQUEST with status
     *       FCGI_CANT_MPX_CONN.
     */
    struct fcgi_data_list_s *datalist = fcgi_data_list_new();
    struct qgis_project_s *project = NULL;

    /* here we do point 1, 2, 3, 4 */
    {

	const char *request_project_name = NULL;

	/* get the maximum read write socket buffer size */
	int maxbufsize = default_max_transfer_buffer_size;
	{
	    int sockbufsize = 0;
	    socklen_t size = sizeof(sockbufsize);
	    retval = getsockopt(inetsocketfd, SOL_SOCKET, SO_RCVBUF, &sockbufsize, &size);
	    if (-1 == retval)
	    {
		logerror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);

	    debug(1, "set maximum transfer buffer to %d\n", maxbufsize);
	}

	char *buffer = malloc(maxbufsize);
	assert(buffer);
	if ( !buffer )
	{
	    logerror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	struct fcgi_session_s *fcgi_session = fcgi_session_new(1);


	int maxfd = 0;
	fd_set rfds;
	int can_read_networksock = 0;


	if (inetsocketfd > maxfd)
	    maxfd = inetsocketfd;

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

	    debug(1, "[%ld] selecting on network connections\n", thread_id);
	    FD_ZERO(&rfds);
	    if ( !can_read_networksock )
		FD_SET(inetsocketfd, &rfds);
	    retval = select(maxfd, &rfds, NULL, NULL, NULL /*&timeout*/);
	    if (-1 == retval)
	    {
		switch (errno)
		{
		case EINTR:
		    /* We received a termination signal.
		     * End this thread, close all file descriptors
		     * and let the main thread clean up.
		     */
		    debug(1, "thread_handle_connection() received interrupt\n");
		    break;

		default:
		    logerror("error: thread_handle_connection() calling select");
		    exit(EXIT_FAILURE);
		    // no break needed
		}
		break;
	    }

	    if (FD_ISSET(inetsocketfd, &rfds))
	    {
		debug(1, "[%ld]  can read from network socket\n", thread_id);
		can_read_networksock = 1;
	    }

	    if (can_read_networksock)
	    {
		debug(1, "[%ld]  read data from network socket: ", thread_id);

		int readbytes = read(inetsocketfd, buffer, maxbufsize);
		debug(1, "read %d, ", readbytes);
		if (-1 == readbytes)
		{
		    logerror("\nerror: reading from network socket");
		    exit(EXIT_FAILURE);
		}
		else if (0 == readbytes)
		{
		    /* end of file received. exit this thread */
		    break;
		}
#ifdef PRINT_NETWORK_DATA
		debug(1, "\n[%ld] network data:\n", thread_id);
		fwrite(buffer, 1, readbytes, stderr);
		debug(1, "\n");
#endif

		{
		    /* allocate data storage here,
		     * delete it in fcgi_data_list_delete()
		     */
		    char *data = malloc(readbytes);
		    assert(data);
		    if ( !data )
		    {
			logerror("could not allocate memory");
			exit(EXIT_FAILURE);
		    }

		    memcpy(data, buffer, readbytes);
		    fcgi_data_add_data(datalist, data, readbytes);

		    fcgi_session_parse(fcgi_session, data, readbytes);

		    enum fcgi_session_state_e session_state = fcgi_session_get_state(fcgi_session);
		    switch (session_state)
		    {
		    case FCGI_SESSION_STATE_PARAMS_DONE:	// fall through
		    case FCGI_SESSION_STATE_END:
			/* we have the parameters complete.
			 * TODO: now look for the URL and assign a process list
			 */
		    {
			int num_proj = config_get_num_projects();
			int i;
			for (i=0; i<num_proj; i++)
			{
			    const char *proj_name = config_get_name_project(i);
			    if (proj_name)
			    {
				const char *key = config_get_scan_parameter_key(proj_name);
				if ( key )
				{
				    const char *param = fcgi_session_get_param(fcgi_session, key);
				    const char *scanregex = config_get_scan_parameter_regex(proj_name);
				    debug(1, "use regex %s\n", scanregex);
				    regex_t regex;
				    /* Compile regular expression */
				    retval = regcomp(&regex, scanregex, REG_EXTENDED);
				    if( retval )
				    {
					size_t len = regerror(retval, &regex, NULL, 0);
					char *buffer = malloc(len);
					(void) regerror (retval, &regex, buffer, len);

					debug(1, "Could not compile regular expression: %s\n", buffer);
					free(buffer);
					exit(EXIT_FAILURE);
				    }

				    /* Execute regular expression */
				    retval = regexec(&regex, param, 0, NULL, 0);
				    if( !retval ){
					// Match
					regfree(&regex);
					request_project_name = proj_name;
					break;
				    }
				    else if( retval == REG_NOMATCH ){
					// No match, go on with next project
				    }
				    else{
					size_t len = regerror(retval, &regex, NULL, 0);
					char *buffer = malloc(len);
					(void) regerror (retval, &regex, buffer, len);

					debug(1, "Could not match regular expression: %s\n", buffer);
					free(buffer);
					exit(EXIT_FAILURE);
				    }

				    regfree(&regex);
				}
				else
				{
				    // TODO: do not overflow the log with this message, do parse the config file at program start
				    debug(1, "error: no regular expression found for project '%s'\n", proj_name);
				}

			    }
			    else
			    {
				debug(1, "error: no name for project number %d in configuration found\n", i);
			    }
			}
			debug(1, "found project '%s' in query string\n", request_project_name);
			has_finished = 1;
			break;
		    }
		    default:
			/* do nothing, parse on.. */
			break;
		    }


		}
		can_read_networksock = 0;
	    }

	}

	requestId = fcgi_session_get_requestid(fcgi_session);
	role = fcgi_session_get_role(fcgi_session);

	free(buffer);
//	fcgi_session_print(fcgi_session);
	fcgi_session_delete(fcgi_session);


	/* find the relevant project by name */
	if (request_project_name)
	    project = find_project_by_name(projectlist, request_project_name);

    }



    struct qgis_process_list_s *proclist = NULL;

    /* here we do point 5 */
    if (project)
    {
	proclist = qgis_project_get_process_list(project);

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
	int proc_state_idle = qgis_process_list_get_num_process_by_status(proclist, PROC_IDLE);
	int proc_state_init = qgis_process_list_get_num_process_by_status(proclist, PROC_INIT);
	int proc_state_start = qgis_process_list_get_num_process_by_status(proclist, PROC_START);
// TODO: use project specific min process number
	int missing_processes = default_min_free_processes - (proc_state_idle + proc_state_init + proc_state_start);
	if (missing_processes > 0)
	{
	    /* not enough free processes, start new ones and add them to the existing processes */
	    qgis_project_start_new_process_detached(missing_processes, project, 0);
	}
    }
    else
    {
	printlog("[%lu] Found no project for request from %s", thread_id, tinfo->hostname);
    }

    /* find the next idling process, set its state to BUSY and attach a thread to it */
    if (proclist)
	proc = qgis_process_list_find_idle_return_busy(proclist);

    if ( !proc )
    {
	/* Found no idle processes.
	 * What now?
	 * All busy, close the network connection.
	 * Sorry guys.
	 */
	printlog("[%lu] Found no free process for network request from %s. Answer overload and close connection\n", thread_id, tinfo->hostname);
	/* NOTE: intentionally no mutex unlock here. We checked all processes,
	 * locked and unlocked all entries. Now there is no locked mutex left.
	 */

	/* We have parsed all incoming messages to get the request id.
	 * Now we answer with an overload status end request. After that we
	 * can close the connection and exit the thread.
	 */


	/* get the maximum read write socket buffer size */
	int maxbufsize = default_max_transfer_buffer_size;
	{
	    int sockbufsize = 0;
	    socklen_t size = sizeof(sockbufsize);
	    retval = getsockopt(inetsocketfd, SOL_SOCKET, SO_SNDBUF, &sockbufsize, &size);
	    if (-1 == retval)
	    {
		logerror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);


	    debug(1, "set maximum transfer buffer to %d\n", maxbufsize);
	}


	int maxfd = 0;
	fd_set wfds;
	int can_write_networksock = 0;


	if (inetsocketfd > maxfd)
	    maxfd = inetsocketfd;

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

	    debug(1, "[%ld] selecting on network connections\n", thread_id);
	    FD_ZERO(&wfds);
	    if ( !can_write_networksock )
		FD_SET(inetsocketfd, &wfds);
	    retval = select(maxfd, NULL, &wfds, NULL, NULL /*&timeout*/);
	    if (-1 == retval)
	    {
		switch (errno)
		{
		case EINTR:
		    /* We received a termination signal.
		     * End this thread, close all file descriptors
		     * and let the main thread clean up.
		     */
		    debug(1, "thread_handle_connection() received interrupt\n");
		    break;

		default:
		    logerror("error: thread_handle_connection() calling select");
		    exit(EXIT_FAILURE);
		    // no break needed
		}
		break;
	    }

	    if (FD_ISSET(inetsocketfd, &wfds))
	    {
		debug(1, "[%ld]  can write to network socket\n", thread_id);
		can_write_networksock = 1;
	    }

	    if (can_write_networksock)
	    {

		/* parse and check the incoming message. If it is a
		 * session initiation message from the web server then
		 * immediately answer with an overload message and end
		 * this thread.
		 */
		if (FCGI_RESPONDER == role)
		{
		    char sendbuffer[sizeof(FCGI_EndRequestRecord)];
		    struct fcgi_message_s *sendmessage = fcgi_message_new_endrequest(requestId, 0, FCGI_OVERLOADED);
		    retval = fcgi_message_write(sendbuffer, sizeof(sendbuffer), sendmessage);

		    int writebytes = write(inetsocketfd, sendbuffer, retval);
		    debug(1, "[%ld] wrote %d\n", thread_id, writebytes);
		    if (-1 == writebytes)
		    {
			logerror("error: writing to network socket");
			exit(EXIT_FAILURE);
		    }

		    /* we are done with the connection. The status
		     * is send back to the web server. we can close
		     * down and leave.
		     */
		    fcgi_message_delete(sendmessage);
		}

		has_finished = 1;
	    }

	}

    }
    /* here we do point 6, 7, 8 */
    else
    {

	{
	    pid_t pid = qgis_process_get_pid(proc);
	    const char *projname = qgis_project_get_name(project);
	    printlog("[%lu] Use process %d to handle request for %s, project %s", thread_id, pid, tinfo->hostname, projname );
	}

	/* change the connection flag of the fastcgi connection to not
	 * FCGI_KEEP_CONN. This way the child process closes the unix socket
	 * if the work is done, and this thread can release its resources.
	 */
	{
	    struct fcgi_data_list_iterator_s *myit = fcgi_data_get_iterator(datalist);
	    assert(myit);
	    const struct fcgi_data_s *fcgidata = fcgi_data_get_next_data(&myit);
	    assert(fcgidata);

	    const char *data = fcgi_data_get_data(fcgidata);
	    int len = fcgi_data_get_datalen(fcgidata);

	    struct fcgi_message_s *message = fcgi_message_new();

	    fcgi_message_parse(message, data, len);
	    int parse_done = fcgi_message_get_parse_done(message);
	    /* If the network connection didn't deliver
	     * sizeof(FCGI_BeginRequestRecord) in one part, the assert
	     * below catches.
	     * In this case we have to write a routine which moves the first
	     * entries of the transmission list (datalist) into one entry.
	     */
	    assert(parse_done > 0);

	    int flag =  fcgi_message_get_flag(message);
	    if (flag >= 0)
	    {
		if (flag & FCGI_KEEP_CONN)
		{
		    flag &= ~FCGI_KEEP_CONN; // delete connection keep flag.
		    fcgi_message_set_flag(message, flag);
		    /* TODO: this does not work, if the first message is not
		     * send complete (i.e. 16 bytes complete). Then the next
		     * command would write into the half complete message buffer
		     * a full complete message, and overwrite the header of
		     * the next message.
		     */
		    fcgi_message_write((char *)data, len, message); // remove constant flag from "data" to write into this buffer
//		    fcgi_message_print(message);
		}
	    }

	    fcgi_message_delete(message);
	}


	int childunixsocketfd;

	/* get the address of the socket transferred to the child process,
	 * then connect to it.
	 */
	childunixsocketfd = qgis_process_get_socketfd(proc);	// refers to the socket the child process accept()s from
	retval = getsockname(childunixsocketfd, &sockaddr, &sockaddrlen);
	if (-1 == retval)
	{
	    logerror("error retrieving the name of child process socket %d", childunixsocketfd);
	    exit(EXIT_FAILURE);
	}
	/* leave the original child socket and create a new one on the opposite
	 * side.
	 */
	retval = socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
	if (-1 == retval)
	{
	    logerror("error: can not create socket to child process");
	    exit(EXIT_FAILURE);
	}
	childunixsocketfd = retval;	// refers to the socket this program connects to the child process
	retval = connect(childunixsocketfd, &sockaddr, sizeof(sockaddr));
	if (-1 == retval)
	{
	    logerror("error: can not connect to child process");
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
		logerror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);

	    size = sizeof(sockbufsize);
	    retval = getsockopt(childunixsocketfd, SOL_SOCKET, SO_RCVBUF, &sockbufsize, &size);
	    if (-1 == retval)
	    {
		logerror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);

	    size = sizeof(sockbufsize);
	    retval = getsockopt(inetsocketfd, SOL_SOCKET, SO_SNDBUF, &sockbufsize, &size);
	    if (-1 == retval)
	    {
		logerror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);

	    size = sizeof(sockbufsize);
	    retval = getsockopt(inetsocketfd, SOL_SOCKET, SO_RCVBUF, &sockbufsize, &size);
	    if (-1 == retval)
	    {
		logerror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);

	    debug(1, "set maximum transfer buffer to %d\n", maxbufsize);

	}
	char *buffer = malloc(maxbufsize);
	assert(buffer);
	if ( !buffer )
	{
	    logerror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}


	/* get an iterator for the already stored messages
	 * then flush the fcgi data queue to the socket interface
	 */
	struct fcgi_data_list_iterator_s *fcgi_data_iterator = fcgi_data_get_iterator(datalist);



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

	    debug(1, "[%ld] selecting on network connections\n", thread_id);
	    FD_ZERO(&rfds);
	    FD_ZERO(&wfds);
	    if ( !can_read_networksock && !fcgi_data_iterator_has_data(fcgi_data_iterator) )
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
		    debug(1, "thread_handle_connection() received interrupt\n");
		    break;

		default:
		    logerror("error: thread_handle_connection() calling select");
		    exit(EXIT_FAILURE);
		    // no break needed
		}
		break;
	    }

	    if (FD_ISSET(inetsocketfd, &wfds))
	    {
		debug(1, "[%ld]  can write to network socket\n", thread_id);
		can_write_networksock = 1;
	    }
	    if (FD_ISSET(childunixsocketfd, &wfds))
	    {
		debug(1, "[%ld]  can write to unix socket\n", thread_id);
		can_write_unixsock = 1;
	    }
	    if (FD_ISSET(inetsocketfd, &rfds))
	    {
		debug(1, "[%ld]  can read from network socket\n", thread_id);
		can_read_networksock = 1;
	    }
	    if (FD_ISSET(childunixsocketfd, &rfds))
	    {
		debug(1, "[%ld]  can read from unix socket\n", thread_id);
		can_read_unixsock = 1;
	    }


	    if ( can_write_unixsock )
	    {
		/* queue data is still available, try to send everything to
		 * the unix socket.
		 */
		if (fcgi_data_iterator_has_data(fcgi_data_iterator))
		{
		    const struct fcgi_data_s *fcgi_data = fcgi_data_get_next_data(&fcgi_data_iterator);

		    const char *data = fcgi_data_get_data(fcgi_data);
		    int datalen = fcgi_data_get_datalen(fcgi_data);
//		    retval = write(debugfd, data, datalen);
		    int writebytes = write(childunixsocketfd, data, datalen);
		    debug(1, "[%ld] wrote %d\n", thread_id, writebytes);
		    if (-1 == writebytes)
		    {
			logerror("error: writing to child process socket");
			exit(EXIT_FAILURE);
		    }
		    can_write_unixsock = 0;
		}
		/* all data from the data queue is flushed.
		 * now we can transfer the data directly from
		 * network to unix socket.
		 */
		else if ( can_read_networksock )
		{
		    debug(1, "[%ld]  read data from network socket: ", thread_id);
		    int readbytes = read(inetsocketfd, buffer, maxbufsize);
		    debug(1, "read %d, ", readbytes);
		    if (-1 == readbytes)
		    {
			logerror("\nerror: reading from network socket");
			exit(EXIT_FAILURE);
		    }
		    else if (0 == readbytes)
		    {
			/* end of file received. exit this thread */
			break;
		    }
#ifdef PRINT_NETWORK_DATA
		    debug(1, "\n[%ld] network data:\n", thread_id);
		    fwrite(buffer, 1, readbytes, stderr);
		    debug(1, "\n");
#endif

		    int writebytes = write(childunixsocketfd, buffer, readbytes);
		    debug(1, "[%ld] wrote %d\n", thread_id, writebytes);
		    if (-1 == writebytes)
		    {
			logerror("error: writing to child process socket");
			exit(EXIT_FAILURE);
		    }
		    can_read_networksock = 0;
		    can_write_unixsock = 0;
		}
	    }

	    if (can_read_unixsock && can_write_networksock)
	    {
		debug(1, "[%ld]  read data from unix socket: ", thread_id);
		int readbytes = read(childunixsocketfd, buffer, maxbufsize);
		debug(1, "read %d, ", readbytes);
		if (-1 == readbytes)
		{
		    logerror("error: reading from child process socket");
		    exit(EXIT_FAILURE);
		}
		else if (0 == readbytes)
		{
		    /* end of file received. exit this thread */
		    break;
		}
#ifdef PRINT_SOCKET_DATA
		debug(1, "fcgi data:\n");
		fwrite(buffer, 1, readbytes, stderr);
		debug(1, "\n");
#endif
		int writebytes = write(inetsocketfd, buffer, readbytes);
		debug(1, "wrote %d\n", writebytes);
		if (-1 == writebytes)
		{
		    logerror("error: writing to network socket");
		    exit(EXIT_FAILURE);
		}

		can_read_unixsock = 0;
		can_write_networksock = 0;
	    }

	}
	retval = close (childunixsocketfd);
	debug(1, "closed child socket fd %d, retval %d, errno %d", childunixsocketfd, retval, errno);
	free(buffer);
	qgis_process_set_state_idle(proc);
    }
//    close(debugfd);


    retval = qgis_timer_stop(&ts);
    if (-1 == retval)
    {
	logerror("clock_gettime(%d,..)", get_valid_clock_id());
	exit(EXIT_FAILURE);
    }
    printlog("[%lu] done connection, %ld.%03ld sec", thread_id, ts.tv_sec, ts.tv_nsec/(1000*1000));


    /* clean up */
    retval = close (inetsocketfd);
    debug(1, "closed internet socket fd %d, retval %d, errno %d", inetsocketfd, retval, errno);
    fcgi_data_list_delete(datalist);
    free(tinfo->hostname);
    free(arg);

    return NULL;
}


struct signal_data_s
{
    int signal;
    union {
	pid_t pid;
    };
};

/* act on signals */
/* TODO: sometimes the program hangs during shutdown.
 *       we receive a signal (SIGCHLD) but dont react on it?
 */
/* TODO: according to this website
 *       http://www.securecoding.cert.org/confluence/display/c/SIG30-C.+Call+only+asynchronous-safe+functions+within+signal+handlers
 *       we should not call fprintf (or vdprintf in log message) from a signal
 *       handler.
 *       Use write in here?
 */
int signalpipe_wr = -1;
void signalaction(int sig, siginfo_t *info, void *ucontext)
{
    int retval;
    struct signal_data_s sigdata;
    sigdata.signal = sig;
    sigdata.pid = info->si_pid;
    debug(1, "got signal %d from pid %d\n", sig, info->si_pid);
    switch (sig)
    {
    case SIGCHLD:
	/* got a child signal. Additionally
	 * call the shutdown handler, maybe it knows what to do with this
	 */
	qgis_shutdown_process_died(info->si_pid);
	// no break

    case SIGTERM:	// fall through
    case SIGINT:	// fall through
    case SIGQUIT:
	/* write signal to main thread in case we are not in the progress
	 * of shutting down */
	if ( !get_program_shutdown() )
	{
	    assert(signalpipe_wr >= 0);
	    retval = write(signalpipe_wr, &sigdata, sizeof(sigdata));
	    if (-1 == retval)
	    {
		logerror("write signal data");
		exit(EXIT_FAILURE);
	    }
	    debug(1, "wrote %d bytes to sig pipe\n", retval);
	}
	break;

    case SIGSEGV:
	printlog("Got SIGSEGV! exiting..");
	syncfs(STDERR_FILENO);
	syncfs(STDOUT_FILENO);
	/* reinstall default handler and fire signal again */
	{
	    struct sigaction action;
	    memset(&action, 0, sizeof(action));
	    action.sa_handler = SIG_DFL;
	    sigaction(SIGSEGV, &action, NULL);
//	    signal(SIGSEGV, SIG_DFL);
	    raise(SIGSEGV);
	}
	break;

    default:
	debug(1, "Huh? Got unexpected signal %d. Ignored\n", sig);
	break;
    }
}


struct thread_start_project_processes_args
{
    struct qgis_project_s *project;
    int num;
};

void *thread_start_project_processes(void *arg)
{
    assert(arg);
    struct thread_start_project_processes_args *targ = arg;
    struct qgis_project_s *project = targ->project;
    int num = targ->num;

    assert(project);
    assert(num >= 0);
    assert(projectlist);

    /* start "num" processes for this project and wait for them to finish
     * its initialization.
     * Then add this project to the global list
     */
    qgis_project_start_new_process_wait(num, project, 0);
    qgis_proj_list_add_project(projectlist, project);


    free(arg);
    return NULL;
}




int main(int argc, char **argv)
{
    int exitvalue = EXIT_SUCCESS;
    int no_daemon = 0;
    int serversocketfd = -1;
    const char *config_path = DEFAULT_CONFIG_PATH;

    int opt;

    while ((opt = getopt(argc, argv, "hdc:V")) != -1)
    {
	switch (opt)
	{
	case 'h':
	    usage(argv[0]);
	    return EXIT_SUCCESS;
	case 'd':
	    no_daemon = 1;
	    break;
	case 'c':
	    config_path = optarg;
	    break;
	case 'V':
	    print_version();
	    return EXIT_SUCCESS;
	default: /* '?' */
	    usage(argv[0]);
	    return EXIT_FAILURE;
	}
    }



    int retval = config_load(config_path);
    if (retval)
    {
	logerror("can not load config file");
	exit(EXIT_FAILURE);
    }


    logger_init();
    printlog("starting %s with pid %d", basename(argv[0]), getpid());
    debug(1, "started main thread");

    test_set_valid_clock_id();

    /* prepare inet socket connection for application server process (this)
     */
    {
	struct addrinfo hints;
	struct addrinfo *result = NULL, *rp = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC; /* Allow IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM; /* TCP socket */
	hints.ai_flags = AI_PASSIVE; /* For wildcard IP address */
	//hints.ai_protocol = 0;          /* Any protocol */
	//hints.ai_canonname = NULL;
	//hints.ai_addr = NULL;
	//hints.ai_next = NULL;

	const char *net_listen = config_get_network_listen();
	const char *net_port = config_get_network_port();
	int s = getaddrinfo(net_listen, net_port, &hints, &result);
	if (s != 0)
	{
	    debug(1, "getaddrinfo: %s\n", gai_strerror(s));
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
		logerror(" could not create socket for network data");
		continue;
	    }

	    int value = 1;
	    int retval = setsockopt(serversocketfd, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value));
	    if (-1 == retval)
	    {
		logerror(" could not set socket to SOL_SOCKET");
	    }

	    if (bind(serversocketfd, rp->ai_addr, rp->ai_addrlen) == 0)
		break; /* Success */

	    //printf(" could not bind to socket\n");
	    logerror(" could not bind to network socket");
	    close(serversocketfd);
	}

	if (rp == NULL)
	{ /* No address succeeded */
	    //debug(1, "Could not bind\n"); // TODO better message
	    logerror("could not create network socket");
	    exit(EXIT_FAILURE);
	}

	freeaddrinfo(result); /* No longer needed */
    }


    /* we are server. listen to incoming connections */
    retval = listen(serversocketfd, SOMAXCONN);
    if (retval)
    {
	logerror("error: can not listen to socket");
	exit(EXIT_FAILURE);
    }


    /* change root directory if requested */
    {
	const char *chrootpath = config_get_chroot();
	if (chrootpath)
	{
	    retval = chroot(chrootpath);
	    if (retval)
	    {
		logerror("error: can not change root directory to '%s'", chrootpath);
		exit(EXIT_FAILURE);
	    }
	}
    }


    /* change uid if requested */
    {
	const char *chuser = config_get_chuser();
	if (chuser)
	{
	    errno = 0;
	    struct passwd *pwnam = getpwnam(chuser);
	    if (pwnam)
	    {
		gid_t gid = pwnam->pw_gid;
		retval = setgid(gid);
		if (retval)
		{
		    logerror("error: can not set gid to %d (%s)", gid, chuser);
		    exit(EXIT_FAILURE);
		}

		uid_t uid = pwnam->pw_uid;
		retval = setuid(uid);
		if (retval)
		{
		    logerror("error: can not set uid to %d (%s)", uid, chuser);
		    exit(EXIT_FAILURE);
		}
	    }
	    else
	    {
		if (errno)
		    logerror("can not get the id of user '%s'", chuser);
		else
		    printlog("can not get the id of user '%s'. exiting", chuser);
		exit(EXIT_FAILURE);
	    }
	}
    }


    {
	/* check if the user can break out of the chroot jail */
	/* TODO linux: check capability CAP_SYS_CHROOT */
	const char *chroot = config_get_chroot();
	if ( chroot )
	{
	    uid_t uid = getuid();
	    uid_t euid = geteuid();
	    if ( !uid || !euid )
		printlog("WARNING: chroot requested but did not set (effective) userid different from root. This renders the chroot useless. Did you forget to set 'chuser'?");
	}
    }


    /* be a good server and change your working directory to root '/'.
     * Each child process may set its own working directory by changing
     * the value of cwd=
     */
    retval = chdir("/");
    if (retval)
    {
	logerror("error: can not change working directory to '/'");
	exit(EXIT_FAILURE);
    }


    if ( !no_daemon )
    {
	retval = daemon(daemon_no_change_dir,daemon_no_close_streams);
	if (retval)
	{
	    logerror("error: can not become daemon");
	    exit(EXIT_FAILURE);
	}
    }


    {
	const char *pidfile = config_get_pid_path();
	if (pidfile)
	{
	    write_pid_file(pidfile);
	}
    }


    /* prepare the signal reception.
     * This way we can start a new child if one has exited on its own,
     * or we can kill the children if this management process got signal
     * to terminate.
     */
    int signalpipe_rd;
    {
	/* create a pipe to send signal status changes to main thread
	 */
	int pipes[2];
	retval = pipe2(pipes, O_CLOEXEC|O_NONBLOCK);
	if (retval)
	{
	    logerror("error: can not install signal pipe");
	    exit(EXIT_FAILURE);
	}
	signalpipe_rd = pipes[0];
	signalpipe_wr = pipes[1];

	struct sigaction action;
	action.sa_sigaction = signalaction;
	/* note: according to man page sigaction (2) in environments
	 * different from linux the SIGCHLD signal may not be delivered
	 * if SA_NOCLDWAIT is set and the child process ends.
	 */
	action.sa_flags = SA_SIGINFO|SA_NOCLDSTOP|SA_NOCLDWAIT;
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGCHLD);
	sigaddset(&action.sa_mask, SIGTERM);
	sigaddset(&action.sa_mask, SIGINT);
	sigaddset(&action.sa_mask, SIGQUIT);
	retval = sigaction(SIGTERM, &action, NULL);
	if (retval)
	{
	    logerror("error: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	retval = sigaction(SIGQUIT, &action, NULL);
	if (retval)
	{
	    logerror("error: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	retval = sigaction(SIGCHLD, &action, NULL);
	if (retval)
	{
	    logerror("error: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	retval = sigaction(SIGINT, &action, NULL);
	if (retval)
	{
	    logerror("error: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	retval = sigaction(SIGSEGV, &action, NULL);
	if (retval)
	{
	    logerror("error: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
    }


    projectlist = qgis_proj_list_new();

    /* start the inotify watch module */
    qgis_inotify_init(projectlist);

    /* start the process shutdown module */
    qgis_shutdown_init();

    /* start the child processes */
    {
	/* do for every project:
	 * (TODO) check every project for correct configured settings.
	 * Start a thread for every project, which in turn starts multiple
	 *  child processes in parallel.
	 * Wait for the project threads to finish.
	 * After that we accept network connections.
	 */

	int num_proj = config_get_num_projects();
	{
	    pthread_t threads[num_proj];
	    int i;
	    for (i=0; i<num_proj; i++)
	    {
		const char *projname = config_get_name_project(i);
		debug(1, "found project '%s'. Startup child processes\n", projname);

		const char *configpath = config_get_project_config_path(projname);
		struct qgis_project_s *project = qgis_project_new(projname, configpath);

		int nr_of_childs_during_startup	= config_get_min_idle_processes(projname);


		struct thread_start_project_processes_args *targs = malloc(sizeof(*targs));
		assert(targs);
		if ( !targs )
		{
		    logerror("could not allocate memory");
		    exit(EXIT_FAILURE);
		}
		targs->project = project;
		targs->num = nr_of_childs_during_startup;

		retval = pthread_create(&threads[i], NULL, thread_start_project_processes, targs);
		if (retval)
		{
		    errno = retval;
		    logerror("error creating thread");
		    exit(EXIT_FAILURE);
		}
	    }

	    for (i=0; i<num_proj; i++)
	    {
		retval = pthread_join(threads[i], NULL);
		if (retval)
		{
		    errno = retval;
		    logerror("error joining thread");
		    exit(EXIT_FAILURE);
		}
	    }
	}
    }



    /* wait for signals of child processes exiting (SIGCHLD) or to terminate
     * this program (SIGTERM, SIGINT) or clients connecting via network to
     * this server.
     */

    fd_set rfds;
    int maxfd = max(serversocketfd,signalpipe_rd);
    maxfd++;


    int has_finished = 0;
    int is_readable_serversocket = 0;
    int is_readable_signalpipe = 0;
    printlog("Initialization done. Waiting for network connection requests..");
    while ( !has_finished )
    {
	/* wait for connections, signals or timeout */
	/* NOTE: I expect a linux behavior over here:
	 * If select() is interrupted by a signal handler, the timeout value
	 * is modified to contain the remaining time.
	 */
	FD_ZERO(&rfds);
	if (!is_readable_serversocket)
	    FD_SET(serversocketfd, &rfds);
	if (!is_readable_signalpipe)
	    FD_SET(signalpipe_rd, &rfds);
	retval = select(maxfd, &rfds, NULL, NULL, NULL);
	if (-1 == retval)
	{
	    switch (errno)
	    {
	    case EINTR:
		/* We received an interrupt, possibly a termination signal.
		 * Let the main thread clean up: Wait for all child processes
		 * to end, close all remaining file descriptors and exit.
		 */
		debug(1, "main() received interrupt\n");
		break;

	    default:
		logerror("error: main() calling select");
		exit(EXIT_FAILURE);
		// no break needed
	    }
	}


	if (retval > 0)
	{
	    if (FD_ISSET(serversocketfd, &rfds))
	    {
		is_readable_serversocket = 1;
		debug(1, "can read from network socket\n");
	    }
	    if (FD_ISSET(signalpipe_rd, &rfds))
	    {
		is_readable_signalpipe = 1;
		debug(1, "can read from pipe\n");
	    }

	    if (is_readable_signalpipe)
	    {
		/* signal has been send to this thread */
		struct signal_data_s sigdata;
		retval = read(signalpipe_rd, &sigdata, sizeof(sigdata));
		if (-1 == retval)
		{
		    logerror("error: reading signal data");
		    exit(EXIT_FAILURE);
		}
		else
		{
		    debug(1, "-- read %d bytes, got signal %d, child %d\n", retval, sigdata.signal, sigdata.pid);

		    /* react on signals */
		    switch (sigdata.signal)
		    {
		    case SIGCHLD:
		    {
			/* child process died, rearrange the project list */
			qgis_proj_list_process_died(projectlist, sigdata.pid);
			break;
		    }
		    case SIGTERM:	// fall through
		    case SIGINT:
		    case SIGQUIT:
			/* termination signal, kill all child processes */
			debug(1, "exit program\n");
			set_program_shutdown(1);
			break;
		    }

		}

		is_readable_signalpipe = 0;
	    }

	    if (is_readable_serversocket)
	    {
		if (!get_program_shutdown())
		{
		    /* connection available */
		    struct sockaddr addr;
		    socklen_t addrlen = sizeof(addr);
		    retval = accept(serversocketfd, &addr, &addrlen);
		    if (-1 == retval)
		    {
			logerror("error: calling accept");
			exit(EXIT_FAILURE);
		    }
		    else
		    {
			int networkfd = retval;

			/* NOTE: aside from the general rule
			 * "malloc() and free() within the same function"
			 * we transfer the responsibility for this memory
			 * to the thread itself.
			 */
			struct thread_connection_handler_args *targs = malloc(sizeof(*targs));
			assert(targs);
			if ( !targs )
			{
			    logerror("could not allocate memory");
			    exit(EXIT_FAILURE);
			}
			targs->new_accepted_inet_fd = networkfd;


			char hbuf[80], sbuf[10];
			int ret = getnameinfo(&addr, addrlen, hbuf, sizeof(hbuf), sbuf,
				sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV);
			if (ret < 0)
			{
			    printlog("error: can not convert host address: %s", gai_strerror(ret));
			    targs->hostname = NULL;
			}
			else
			{
			    //printlog("Accepted connection from host %s, port %s", hbuf, sbuf);
			    targs->hostname = strdup(hbuf);
			}


			pthread_t thread;
			retval = pthread_create(&thread, NULL, thread_handle_connection, targs);
			if (retval)
			{
			    errno = retval;
			    logerror("error creating thread");
			    exit(EXIT_FAILURE);
			}

			if (targs->hostname)
			{
			    printlog("Accepted connection from host %s, port %s. Handle connection in thread [%lu]", hbuf, sbuf, thread);
			}

		    }

		    is_readable_serversocket = 0;
		}
	    }
	}

	/* over here I expect the main thread to continue AFTER the signal
	 * handler has ended its thread.
	 * If this expectation does not fulfill we have to look for a different
	 * design in this section.
	 */
	if ( get_program_shutdown() )
	{
	    /* we received a termination signal.
	     * reinstall the default signal handler,
	     * empty the pipe (if some other signals have arrived)
	     * and exit this loop
	     */
	    struct sigaction action;
	    memset(&action, 0, sizeof(action));
	    action.sa_handler = SIG_DFL;

	    retval = sigaction(SIGTERM, &action, NULL);
	    if (retval)
	    {
		logerror("error: can not install signal handler");
		exit(EXIT_FAILURE);
	    }
	    retval = sigaction(SIGQUIT, &action, NULL);
	    if (retval)
	    {
		logerror("error: can not install signal handler");
		exit(EXIT_FAILURE);
	    }
	    retval = sigaction(SIGINT, &action, NULL);
	    if (retval)
	    {
		logerror("error: can not install signal handler");
		exit(EXIT_FAILURE);
	    }


	    /* note: we do not need to empty the signal pipe. Either it
	     * contains a signal of SIGTERM, SIGINT, SIGQUIT or it
	     * contains a signal of SIGCHLD.
	     * In the former case we already know that we have to shut down.
	     * In the latter case we get to know the child which has exited
	     * during the clean up sequence.
	     */

	    /* close the pipe */
	    close(signalpipe_rd);
	    close(signalpipe_wr);
	    signalpipe_rd = signalpipe_wr = -1;

	    /* exit the main loop */
	    break;
	}

    }


    debug(1, "closing network socket\n");
    fflush(stderr);
    retval = close(serversocketfd);
    debug(1, "closed internet server socket fd %d, retval %d, errno %d", serversocketfd, retval, errno);

    /* close the inotify module, so no processes are recreated afterwards
     * because of a change in the configuration.
     */
    qgis_inotify_delete();

    /* move the processes from the working lists to the shutdown module */
    qgis_proj_list_shutdown(projectlist);


    /* remove the projects */
    qgis_proj_list_delete(projectlist);

    /* wait for the shutdown module so it has closed all its processes
     * Then clean up the module */
    qgis_shutdown_wait_empty();
    qgis_shutdown_delete();

    {
	const char *pidfile = config_get_pid_path();
	if (pidfile)
	{
	    remove_pid_file(pidfile);
	}
    }
    config_shutdown();
    printlog("shut down %s", basename(argv[0]));

    return exitvalue;
}
