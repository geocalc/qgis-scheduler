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

#include "qgis_process.h"
#include "qgis_process_list.h"
#include "fcgi_state.h"
#include "qgis_config.h"


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


struct thread_init_new_child_args
{
    struct qgis_process_s *proc;
};

struct thread_connection_handler_args
{
    int new_accepted_inet_fd;
};

struct thread_start_new_child_args
{
    struct qgis_process_list_s *list;
};


static const char base_socket_desc[] = "qgis-schedulerd-socket";
static const int default_max_transfer_buffer_size = 4*1024; //INT_MAX;
static const int default_min_free_processes = 1;
//static const char DEFAULT_CONFIG_PATH[] = "/etc/qgis-scheduler/qgis-scheduler.conf";
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
    //fprintf(stdout, "usage: %s [-h] [-d] [-c <CONFIGFILE>]\n", basename(argv0));
    fprintf(stdout, "usage: %s [-h] [-d]\n", basename(argv0));
    fprintf(stdout, "\t-h: print this help\n");
    fprintf(stdout, "\t-d: do NOT become daemon\n");
    fprintf(stdout, "\t-c: use CONFIGFILE (default '%s')\n", DEFAULT_CONFIG_PATH);
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


const char *command = NULL;
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
    assert(arg);
    struct thread_start_new_child_args *tinfo = arg;
    struct qgis_process_list_s *proclist = tinfo->list;
    const char *command = config_get_process( NULL );
//    const char *args = config_get_process_args( NULL );

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

	/* change working dir
	 * close file descriptor stdin = 0
	 * assign socket file descriptor to fd 0
	 * fork
	 * exec
	 */
	// TODO: change "NULL" to real project name
	int ret = chdir(config_get_working_directory(NULL));
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
//	const char *command = iniparser_getstring(ini, CGI_PATH_KEY,
//		CGI_PATH_KEY_DEFAULT);


	/* close all file descriptors different from 0. The fd different from
	 * 1 and 2 are opened during open() and socket() calls with FD_CLOEXEC
	 * flag enabled. This way, all fds are closed during exec() call.
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
	assert(targs);
	if ( !targs )
	{
	    perror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	targs->proc = childproc;
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
	targs->list = proclist;

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

struct fcgi_data_s
{
    char *data;
    int len;
};

struct fcgi_data_list_iterator_s
{
    TAILQ_ENTRY(fcgi_data_list_iterator_s) entries;          /* Linked list prev./next entry */
    struct fcgi_data_s data;
};

struct fcgi_data_list_s
{
    TAILQ_HEAD(data_listhead_s, fcgi_data_list_iterator_s) head;	/* Linked list head */
};


static struct fcgi_data_list_s *fcgi_data_list_new(void)
{
    struct fcgi_data_list_s *list = calloc(1, sizeof(*list));
    assert(list);
    if ( !list )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    return list;
}


static void fcgi_data_list_delete(struct fcgi_data_list_s *datalist)
{
    if (datalist)
    {
	while (datalist->head.tqh_first != NULL)
	{
	    struct fcgi_data_list_iterator_s *entry = datalist->head.tqh_first;

	    TAILQ_REMOVE(&datalist->head, datalist->head.tqh_first, entries);
	    free(entry->data.data);
	    free(entry);
	}

	free(datalist);
    }
}


static void fcgi_data_add_data(struct fcgi_data_list_s *datalist, char *data, int len)
{
    assert(datalist);
    if(datalist)
    {
	struct fcgi_data_list_iterator_s *entry = malloc(sizeof(*entry));
	assert(entry);
	if ( !entry )
	{
	    perror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}

	entry->data.data = data;
	entry->data.len = len;

	/* if list is empty we have to insert at beginning,
	 * else insert at the end.
	 */
	if (datalist->head.tqh_first)
	    TAILQ_INSERT_TAIL(&datalist->head, entry, entries);      /* Insert at the end. */
	else
	    TAILQ_INSERT_HEAD(&datalist->head, entry, entries);
    }

}


static struct fcgi_data_list_iterator_s *fcgi_data_get_iterator(struct fcgi_data_list_s *list)
{
    assert(list);
    if (list)
    {
	return list->head.tqh_first;
    }

    return NULL;
}


static const struct fcgi_data_s *fcgi_data_get_next_data(struct fcgi_data_list_iterator_s **iterator)
{
    assert(iterator);
    if (iterator)
    {
	if (*iterator)
	{
	    struct fcgi_data_s *data = &(*iterator)->data;
	    *iterator = (*iterator)->entries.tqe_next;
	    return data;
	}
    }

    return NULL;
}


static int fcgi_data_iterator_has_data(const struct fcgi_data_list_iterator_s *iterator)
{
    int retval = 0;
    if (iterator)
    {
	retval = 1;
    }

    return retval;
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
    int requestId;
    int role;

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

    fprintf(stderr, "start a new connection thread\n");


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
    const char *request_project_name = NULL;

    /* here we do point 1, 2, 3, 4 */
    {

	/* get the maximum read write socket buffer size */
	int maxbufsize = default_max_transfer_buffer_size;
	{
	    int sockbufsize = 0;
	    socklen_t size = sizeof(sockbufsize);
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
	if ( !buffer )
	{
	    perror("could not allocate memory");
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

	    fprintf(stderr, "[%ld] selecting on network connections\n", thread_id);
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
		    fprintf(stderr, "thread_handle_connection() received interrupt\n");
		    break;

		default:
		    perror("error: thread_handle_connection() calling select");
		    exit(EXIT_FAILURE);
		    // no break needed
		}
		break;
	    }

	    if (FD_ISSET(inetsocketfd, &rfds))
	    {
		fprintf(stderr, "[%ld]  can read from network socket\n", thread_id);
		can_read_networksock = 1;
	    }

	    if (can_read_networksock)
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

		{
		    /* allocate data storage here,
		     * delete it in fcgi_data_list_delete()
		     */
		    char *data = malloc(readbytes);
		    assert(data);
		    if ( !data )
		    {
			perror("could not allocate memory");
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
				    fprintf(stderr, "use regex %s\n", scanregex);
				    regex_t regex;
				    /* Compile regular expression */
				    retval = regcomp(&regex, scanregex, REG_EXTENDED);
				    if( retval )
				    {
					size_t len = regerror(retval, &regex, NULL, 0);
					char *buffer = malloc(len);
					(void) regerror (retval, &regex, buffer, len);

					fprintf(stderr, "Could not compile regular expression: %s\n", buffer);
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

					fprintf(stderr, "Could not match regular expression: %s\n", buffer);
					free(buffer);
					exit(EXIT_FAILURE);
				    }

				    regfree(&regex);
				}
				else
				{
				    // TODO: do not overflow the log with this message, do parse the config file at program start
				    fprintf(stderr, "error: no regular expression found for project '%s'\n", proj_name);
				}

			    }
			    else
			    {
				fprintf(stderr, "error: no name for project number %d in configuration found\n", i);
			    }
			}
			fprintf(stderr, "found project '%s' in query string\n", request_project_name);
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

    }





    /* here we do point 5 */
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
	int proc_state_idle = qgis_process_list_get_num_process_by_status(proclist, PROC_IDLE);
	int proc_state_init = qgis_process_list_get_num_process_by_status(proclist, PROC_INIT);
	int proc_state_start = qgis_process_list_get_num_process_by_status(proclist, PROC_START);

	int missing_processes = default_min_free_processes - (proc_state_idle + proc_state_init + proc_state_start);
	if (missing_processes > 0)
	{
	    /* not enough free processes, start new ones */
	    start_new_process_detached(missing_processes);
	}
    }

    /* find the next idling process and attach a thread to it */
    proc = qgis_process_list_mutex_find_process_by_status(proclist, PROC_IDLE);
    if ( !proc )
    {
	/* Found no idle processes.
	 * What now?
	 * All busy, close the network connection.
	 * Sorry guys.
	 */
	fprintf(stderr, "found no free process for network request. answer overload and close connection\n");
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
		perror("error: getsockopt");
		exit(EXIT_FAILURE);
	    }
	    maxbufsize = min(sockbufsize, maxbufsize);


	    fprintf(stderr, "set maximum transfer buffer to %d\n", maxbufsize);
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

	    fprintf(stderr, "[%ld] selecting on network connections\n", thread_id);
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

	    if (can_write_networksock)
	    {

		/* parse and check the incoming message. If it is a
		 * session initiation message from the web server then
		 * immediately answer with an overload message and end
		 * this thread.
		 */
		if (FCGI_RESPONDER == role)
		{
		    unsigned char sendbuffer[sizeof(FCGI_EndRequestRecord)];
		    struct fcgi_message_s *sendmessage = fcgi_message_new_endrequest(requestId, 0, FCGI_OVERLOADED);
		    retval = fcgi_message_write(sendbuffer, sizeof(sendbuffer), sendmessage);

		    int writebytes = write(inetsocketfd, sendbuffer, retval);
		    fprintf(stderr, "[%ld] wrote %d\n", thread_id, writebytes);
		    if (-1 == writebytes)
		    {
			perror("error: writing to network socket");
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
	/* change the state of process to BUSY
	 * and unlock the mutex protection
	 */
	qgis_process_set_state_busy(proc, thread_id);
	pthread_mutex_t *mutex = qgis_process_get_mutex(proc);
	retval = pthread_mutex_unlock(mutex);
	if (retval)
	{
	    errno = retval;
	    perror("error unlock mutex");
	    exit(EXIT_FAILURE);
	}
	mutex = NULL;


	/* change the connection flag of the fastcgi connection to not
	 * FCGI_KEEP_CONN. This way the child process closes the unix socket
	 * if the work is done, and this thread can release its resources.
	 */
	{
	    struct fcgi_data_list_iterator_s *myit = fcgi_data_get_iterator(datalist);
	    assert(myit);
	    char *data = myit->data.data;
	    int len = myit->data.len;
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
		    fcgi_message_write((unsigned char *)data, len, message);
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
	if ( !buffer )
	{
	    perror("could not allocate memory");
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

	    fprintf(stderr, "[%ld] selecting on network connections\n", thread_id);
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


	    if ( can_write_unixsock )
	    {
		/* queue data is still available, try to send everything to
		 * the unix socket.
		 */
		if (fcgi_data_iterator_has_data(fcgi_data_iterator))
		{
		    const struct fcgi_data_s *fcgi_data = fcgi_data_get_next_data(&fcgi_data_iterator);

		    char *data = fcgi_data->data;
		    int datalen = fcgi_data->len;
		    int writebytes = write(childunixsocketfd, data, datalen);
		    fprintf(stderr, "[%ld] wrote %d\n", thread_id, writebytes);
		    if (-1 == writebytes)
		    {
			perror("error: writing to child process socket");
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
    fcgi_data_list_delete(datalist);
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
    int no_daemon = 0;
    int serversocketfd = -1;
    const char *config_path = DEFAULT_CONFIG_PATH;

    int opt;

    while ((opt = getopt(argc, argv, "hdc:")) != -1)
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
	default: /* '?' */
	    usage(argv[0]);
	    return EXIT_FAILURE;
	}
    }

//    if (optind >= argc)
//    {
//	printf("error: missing command\n");
//	usage(argv[0]);
//	return EXIT_FAILURE;
//    }
//    command = argv[optind++];


    int retval = config_load(config_path);
    if (retval)
    {
	perror("can not load config file");
	exit(EXIT_FAILURE);
    }





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

	    int value = 1;
	    int retval = setsockopt(serversocketfd, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value));
	    if (-1 == retval)
	    {
		perror(" could not set socket to SOL_SOCKET");
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
    retval = listen(serversocketfd, SOMAXCONN);
    if (retval)
    {
	perror("error: can not listen to socket");
	exit(EXIT_FAILURE);
    }


    /* be a good server and change your working directory to root '/'.
     * Each child process may set its own working directory by changing
     * the value of cwd=
     */
    retval = chdir("/");
    if (retval)
    {
	perror("error: can not change working directory to '/'");
	exit(EXIT_FAILURE);
    }


    if ( !no_daemon )
    {
	retval = daemon(daemon_no_change_dir,daemon_no_close_streams);
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
    int i;
    {
	int nr_of_childs_during_startup	= config_get_min_idle_processes(NULL);

	proclist = qgis_process_list_new();
	pthread_t threads[nr_of_childs_during_startup];
	for (i=0; i<nr_of_childs_during_startup; i++)
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
	    targs->list = proclist;

	    retval = pthread_create(&threads[i], NULL, thread_start_new_child, targs);
	    if (retval)
	    {
		errno = retval;
		perror("error creating thread");
		exit(EXIT_FAILURE);
	    }
	}
	for (i=0; i<nr_of_childs_during_startup; i++)
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
		    assert(targs);
		    if ( !targs )
		    {
			perror("could not allocate memory");
			exit(EXIT_FAILURE);
		    }
		    targs->new_accepted_inet_fd = networkfd;

		    pthread_t thread;
		    retval = pthread_create(&thread, NULL, thread_handle_connection, targs);
		    if (retval)
		    {
			errno = retval;
			perror("error creating thread");
			exit(EXIT_FAILURE);
		    }

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
    config_shutdown();

    return exitvalue;
}
