/*
 * connection_manager.c
 *
 *  Created on: 04.03.2016
 *      Author: jh
 */

/*
    Management module for the worker connections.
    Acts on connection events to fulfill the request.
    Connections are done via network fcgi.

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


#include "connection_manager.h"

#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <fastcgi.h>
#include <regex.h>
#include <pthread.h>

#include "database.h"
#include "logger.h"
#include "timer.h"
#include "fcgi_data.h"
#include "fcgi_state.h"
#include "qgis_config.h"
#include "statistic.h"
#include "project_manager.h"


#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })


struct thread_connection_handler_args
{
    int new_accepted_inet_fd;
    char *hostname;
};


static const int default_max_transfer_buffer_size = 4*1024; //INT_MAX;
static const int max_wait_for_idle_process = 5;

static int change_file_mode_blocking(int fd, int is_blocking)
{
    assert(fd>=0);

    int retval = fcntl(fd, F_GETFL, 0);
    if (-1 == retval)
    {
	logerror("error: fcntl(%d, F_GETFL, 0)", fd);
	exit(EXIT_FAILURE);
    }
    int flags = retval;
    debug(1, "got fd %d flags %#x", fd, flags);

    if (is_blocking)
	flags &= ~O_NONBLOCK;
    else
	flags |= O_NONBLOCK;

    retval = fcntl(fd, F_SETFL, flags);
    if (-1 == retval)
    {
	logerror("error: fcntl(%d, F_SETFL, %#x)", fd, flags);
	exit(EXIT_FAILURE);
    }
    debug(1, "set fd %d flags %#x", fd, flags);

    return retval;
}




static void *thread_handle_connection(void *arg)
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

    debug(1, "start a new connection thread");

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

    /* here we do point 1, 2, 3, 4 */
	const char *request_project_name = NULL;
    {


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

	    debug(1, "set maximum transfer buffer to %d", maxbufsize);
	}

	char *buffer = malloc(maxbufsize);
	assert(buffer);
	if ( !buffer )
	{
	    logerror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	struct fcgi_session_s *fcgi_session = fcgi_session_new(1);


	/* set read to blocking mode */
	retval = change_file_mode_blocking(inetsocketfd, 1);

	int has_finished = 0;
	while ( !has_finished )
	{
	    /* wait for connection data */
		debug(1, "read data from network socket: ");

		int readbytes = read(inetsocketfd, buffer, maxbufsize);
		debug(1, "read %d", readbytes);
		if (-1 == readbytes)
		{
		    logerror("error: reading from network socket");
		    exit(EXIT_FAILURE);
		}
		else if (0 == readbytes)
		{
		    /* end of file received. exit this thread */
		    break;
		}
#ifdef PRINT_NETWORK_DATA
		debug(1, "network data:");
		fwrite(buffer, 1, readbytes, stderr);
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
				    debug(1, "use regex %s", scanregex);
				    regex_t regex;
				    /* Compile regular expression */
				    retval = regcomp(&regex, scanregex, REG_EXTENDED);
				    if( retval )
				    {
					size_t len = regerror(retval, &regex, NULL, 0);
					char *buffer = malloc(len);
					(void) regerror (retval, &regex, buffer, len);

					debug(1, "Could not compile regular expression: %s", buffer);
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

					debug(1, "Could not match regular expression: %s", buffer);
					free(buffer);
					exit(EXIT_FAILURE);
				    }

				    regfree(&regex);
				}
				else
				{
				    // TODO: do not overflow the log with this message, do parse the config file at program start
				    debug(1, "error: no regular expression found for project '%s'", proj_name);
				}

			    }
			    else
			    {
				debug(1, "error: no name for project number %d in configuration found", i);
			    }
			}
			debug(1, "found project '%s' in query string", request_project_name);
			has_finished = 1;

			break;
		    }
		    default:
			/* do nothing, parse on.. */
			break;
		    }


		}

	}

	requestId = fcgi_session_get_requestid(fcgi_session);
	role = fcgi_session_get_role(fcgi_session);

	free(buffer);
//	fcgi_session_print(fcgi_session);
	fcgi_session_delete(fcgi_session);

    }


    /* here we do point 5 */
    if (request_project_name)
    {

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
	const char *projname = request_project_name;
	int min_free_processes = config_get_min_idle_processes(projname);

	// TODO make a specialized call only once
	int proc_state_idle = db_get_num_process_by_status(projname, PROC_STATE_IDLE);
	int proc_state_init = db_get_num_process_by_status(projname, PROC_STATE_INIT);
	int proc_state_start = db_get_num_process_by_status(projname, PROC_STATE_START);

	int missing_processes = min_free_processes - (proc_state_idle + proc_state_init + proc_state_start);
	if (missing_processes > 0)
	{
	    /* not enough free processes, start new ones and add them to the existing processes */
	    debug(1, "not enough processes for project %s, start %d new process", projname, missing_processes);
	    project_manager_start_new_process_detached(missing_processes, projname, 0);
	}
    }
    else
    {
	printlog("[%lu] Found no project for request from %s", thread_id, tinfo->hostname);
    }

    /* find the next idling process, set its state to BUSY and attach a thread to it.
     * try at most 5 seconds long to find an idle process */
    /* TODO: better use pthread_condition_signal with timeout */
    pid_t mypid;
    {
	int i;
	for (i=0; i<=max_wait_for_idle_process; i++)
	{
	    mypid = db_get_next_idle_process_for_busy_work(request_project_name);
	    if (mypid>=0 || i>=max_wait_for_idle_process)
		break;
	    else
		sleep(1);
	}
    }

    if ( mypid<0 )
    {
	/* Found no idle processes.
	 * What now?
	 * All busy, close the network connection.
	 * Sorry guys.
	 */
	printlog("[%lu] Found no free process for network request from %s. Answer overload and close connection", thread_id, tinfo->hostname);
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


	    debug(1, "set maximum transfer buffer to %d", maxbufsize);
	}



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
	    debug(1, "wrote %d btes to network socket", writebytes);
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


    }
    /* here we do point 6, 7, 8 */
    else
    {

	{
	    pid_t pid = mypid;
	    const char *projname = request_project_name;
	    printlog("[%lu] Use process %d to handle request for %s, project %s", thread_id, pid, tinfo->hostname, projname );
	}

	/* set read to non-blocking mode */
	retval = change_file_mode_blocking(inetsocketfd, 0);

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
	childunixsocketfd = db_get_process_socket(mypid);//qgis_process_get_socketfd(proc);	// refers to the socket the child process accept()s from
	retval = getsockname(childunixsocketfd, (struct sockaddr *)&sockaddr, &sockaddrlen);
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
	retval = connect(childunixsocketfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr));
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

	    debug(1, "set maximum transfer buffer to %d", maxbufsize);

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


	enum {
	    networkfd_slot = 0,
	    unixfd_slot = 1,
	    num_poll_slots
	};
	struct pollfd pfd[num_poll_slots];
	pfd[networkfd_slot].fd = inetsocketfd;
	pfd[unixfd_slot].fd = childunixsocketfd;

	int can_read_networksock = 0;
	int can_write_networksock = 0;
	int can_read_unixsock = 0;
	int can_write_unixsock = 0;

	int has_finished = 0;
	while ( !has_finished )
	{
	    /* wait for connection data */
	    debug(1, "poll on network connections");
	    pfd[networkfd_slot].events = 0;
	    pfd[unixfd_slot].events = 0;
	    if ( !can_read_networksock && !fcgi_data_iterator_has_data(fcgi_data_iterator) )
		pfd[networkfd_slot].events |= POLLIN;
	    if ( !can_write_networksock )
		pfd[networkfd_slot].events |= POLLOUT;
	    if ( !can_read_unixsock )
		pfd[unixfd_slot].events |= POLLIN;
	    if ( !can_write_unixsock )
		pfd[unixfd_slot].events |= POLLOUT;

	    retval = poll(pfd, num_poll_slots, -1);
	    if (-1 == retval)
	    {
		switch (errno)
		{
		case EINTR:
		    /* We received a termination signal.
		     * End this thread, close all file descriptors
		     * and let the main thread clean up.
		     */
		    debug(1, "received interrupt");
		    break;

		default:
		    logerror("error: %s() calling poll", __FUNCTION__);
		    exit(EXIT_FAILURE);
		    // no break needed
		}
		break;
	    }

	    if (POLLOUT & pfd[networkfd_slot].revents)
	    {
		debug(1, "can write to network socket");
		can_write_networksock = 1;
	    }
	    if (POLLOUT & pfd[unixfd_slot].revents)
	    {
		debug(1, "can write to unix socket");
		can_write_unixsock = 1;
	    }
	    if (POLLIN & pfd[networkfd_slot].revents)
	    {
		debug(1, "can read from network socket");
		can_read_networksock = 1;
	    }
	    if (POLLIN & pfd[unixfd_slot].revents)
	    {
		debug(1, "can read from unix socket");
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
		    debug(1, "wrote %d", writebytes);
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
		    debug(1, "read data from network socket: ");
		    int readbytes = read(inetsocketfd, buffer, maxbufsize);
		    debug(1, "read %d, ", readbytes);
		    if (-1 == readbytes)
		    {
			if (ECONNRESET == errno)
			{
			    /* network client ended this connection. exit this thread */
			    debug(1, "errno %d, connection reset by peer, closing connection", errno);
			    break;
			}
			else
			{
			    logerror("error: reading from network socket (%d)", errno);
			    exit(EXIT_FAILURE);
			}
		    }
		    else if (0 == readbytes)
		    {
			/* end of file received. exit this thread */
			break;
		    }
#ifdef PRINT_NETWORK_DATA
		    debug(1, "network data:");
		    fwrite(buffer, 1, readbytes, stderr);
#endif

		    int writebytes = write(childunixsocketfd, buffer, readbytes);
		    debug(1, "wrote %d", writebytes);
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
		debug(1, "read data from unix socket: ");
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
		debug(1, "fcgi data:");
		fwrite(buffer, 1, readbytes, stderr);
#endif
		int writebytes = write(inetsocketfd, buffer, readbytes);
		debug(1, "wrote %d", writebytes);
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

	db_process_set_state_idle(mypid);

    }
//    close(debugfd);


    retval = qgis_timer_stop(&ts);
    if (-1 == retval)
    {
	logerror("clock_gettime(%d,..)", get_valid_clock_id());
	exit(EXIT_FAILURE);
    }
    printlog("[%lu] done connection, %ld.%03ld sec", thread_id, ts.tv_sec, ts.tv_nsec/(1000*1000));
    statistic_add_connection(&ts);


    /* clean up */
    retval = close (inetsocketfd);
    debug(1, "closed internet socket fd %d, retval %d, errno %d", inetsocketfd, retval, errno);
    fcgi_data_list_delete(datalist);
    free(tinfo->hostname);
    free(arg);

    return NULL;
}


void connection_manager_handle_connection_request(int netfd, const struct sockaddr *addr, unsigned int length)
{
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
	targs->new_accepted_inet_fd = netfd;


	char hbuf[80], sbuf[10];
	int ret = getnameinfo(addr, length, hbuf, sizeof(hbuf), sbuf,
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
	int retval = pthread_create(&thread, NULL, thread_handle_connection, targs);
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
