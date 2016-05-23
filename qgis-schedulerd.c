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



#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <sys/queue.h>
#include <fastcgi.h>
#include <regex.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "common.h"
#include "fcgi_state.h"
#include "fcgi_data.h"
#include "qgis_config.h"
#include "qgis_inotify.h"
#include "logger.h"
#include "timer.h"
#include "qgis_shutdown_queue.h"
#include "statistic.h"
#include "database.h"
#include "process_manager.h"
#include "project_manager.h"
#include "connection_manager.h"




//#define PRINT_NETWORK_DATA
//#define PRINT_SOCKET_DATA
#ifndef DEFAULT_CONFIG_PATH
# define DEFAULT_CONFIG_PATH	"/etc/qgis-scheduler/qgis-scheduler.conf"
#endif





static const char version[] = VERSION;
static const int daemon_no_change_dir = 0;
static const int daemon_no_close_streams = 1;



#ifndef HAVE_BASENAME
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


/* this scheduler needs a whole lot of file descriptors.
 * check the number of possible open fd and adopt the limit settings to it.
 */
void check_ressource_limits(void)
{
    struct rlimit limits;

    int retval = getrlimit(RLIMIT_NOFILE, &limits);
    if (retval)
    {
	logerror("ERROR: can not get ressource limits");
	exit(EXIT_FAILURE);
    }

    const unsigned long fdlimit = limits.rlim_cur;
    const unsigned long fdmax = limits.rlim_max;
    debug(1, "got fd limit %lu - max limit %lu", fdlimit, fdmax);

    /* calculated amount of open file descriptors needed:
     * number of projects * 2 open sockets * ~20 fds per child process
     *  + number of connection threads (= number of projects * number of processes)
     *  + 2 logfile + 1 network listener
     * e.g. 23 projects => ~20 *2 *23 + ~3 *23 +2 +3 = 996
     * Open file descriptors needed outside this program is ~900-1000
     * (measured with "lsof").
     */
    const int num_projects = config_get_num_projects();
    int num_processes = 0;
    int i;
    for (i=0; i<num_projects; i++)
    {
	const char *proj_name = config_get_name_project(i);
	const int max_proc = config_get_max_idle_processes(proj_name);
	num_processes += max_proc;
    }
    const unsigned long fdlimit_needed = num_projects *2 *20 +num_processes +7 + 950;
    debug(1, "calculated needed fd limit %lu", fdlimit_needed);

    if (fdlimit < fdlimit_needed)
    {
	printlog("WARNING: too low max limit of open files = %lu. Setting limit to %lu. Consider changing \"soft nofile\" entry in /etc/security/limits.conf to %lu or more", fdlimit, fdlimit_needed, fdlimit_needed);

	limits.rlim_cur = fdlimit_needed;
	retval = setrlimit(RLIMIT_NOFILE, &limits);
	if (retval)
	{
	    logerror("ERROR: can not set ressource limits");
	    exit(EXIT_FAILURE);
	}
    }
}




struct signal_data_s
{
    int signal;
    pid_t pid;
    int is_shutdown;
};

/* act on signals */
/* TODO: according to this website
 *       http://www.securecoding.cert.org/confluence/display/c/SIG30-C.+Call+only+asynchronous-safe+functions+within+signal+handlers
 *       we should not call fprintf (or vdprintf in log message) from a signal
 *       handler.
 *       Use write in here?
 */
int signalpipe_wr = -1;
void signalaction(int sig, siginfo_t *info, void *ucontext)
{
    UNUSED_PARAMETER(ucontext);

    int retval;
    struct signal_data_s sigdata;
    sigdata.signal = sig;
    sigdata.pid = info->si_pid;
    sigdata.is_shutdown = 0;
    debug(1, "got signal %d from pid %d", sig, info->si_pid);
    switch (sig)
    {
    case SIGCHLD:	// fall through
    case SIGHUP:	// fall through
    case SIGUSR1:	// fall through
    case SIGUSR2:	// fall through
    case SIGTERM:	// fall through
    case SIGINT:	// fall through
    case SIGQUIT:
	/* write signal to main thread */
	assert(signalpipe_wr >= 0);
	retval = write(signalpipe_wr, &sigdata, sizeof(sigdata));
	if (-1 == retval)
	{
	    logerror("write signal data");
	    exit(EXIT_FAILURE);
	}
	debug(1, "wrote %d bytes to sig pipe", retval);
	break;

    case SIGSEGV:
	printlog("Got SIGSEGV! exiting..");
	syncfs(STDERR_FILENO);
	syncfs(STDOUT_FILENO);
	/* reinstall default handler and fire signal again */
	/* update: reinstalled upon entry by action.flag SA_RESETHAND */
	raise(SIGSEGV);
	break;

    default:
	debug(1, "Huh? Got unexpected signal %d. Ignored", sig);
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


    test_set_valid_clock_id();
    statistic_init();

    /* convert relative path given to an absolute path */
    char *configuration_path = realpath(config_path, NULL);
    if (configuration_path)
    {
	config_path = configuration_path;
    }
    else
    {
	logerror("can not canonicalize path '%s'", config_path);
	exit(EXIT_FAILURE);
    }


    char **sectionnew, **sectionchange, **sectiondelete;
    int retval = config_load(configuration_path, &sectionnew, &sectionchange, &sectiondelete);
    if (retval)
    {
	logerror("can not load config file");
	exit(EXIT_FAILURE);
    }


    logger_init();
    printlog("starting %s version %s with pid %d", basename(argv[0]), version, getpid());
    debug(1, "started main thread");

    check_ressource_limits();

    db_init();

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
	    debug(1, "getaddrinfo: %s", gai_strerror(s));
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
	    //debug(1, "Could not bind"); // TODO better message
	    logerror("could not create network socket");
	    exit(EXIT_FAILURE);
	}

	freeaddrinfo(result); /* No longer needed */
    }


    /* we are server. listen to incoming connections */
    retval = listen(serversocketfd, SOMAXCONN);
    if (retval)
    {
	logerror("ERROR: can not listen to socket");
	exit(EXIT_FAILURE);
    }


    /* change root directory if requested */
    {
	const char *chrootpath = config_get_chroot();
	if (chrootpath)
	{
	    /* check if the user can break out of the chroot jail */
	    /* TODO linux: check capability CAP_SYS_CHROOT */
	    const char *chuser = config_get_chuser();
	    if (!chuser)
		printlog("WARNING: chroot requested but did not configure a different userid. This renders the chroot useless. Did you forget to set 'chuser'?");

	    retval = chroot(chrootpath);
	    if (retval)
	    {
		logerror("ERROR: can not change root directory to '%s'", chrootpath);
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
		    logerror("ERROR: can not set gid to %d (%s)", gid, chuser);
		    exit(EXIT_FAILURE);
		}

		uid_t uid = pwnam->pw_uid;
		retval = setuid(uid);
		if (retval)
		{
		    logerror("ERROR: can not set uid to %d (%s)", uid, chuser);
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


    /* be a good server and change your working directory to root '/'.
     * Each child process may set its own working directory by changing
     * the value of cwd=
     */
    retval = chdir("/");
    if (retval)
    {
	logerror("ERROR: can not change working directory to '/'");
	exit(EXIT_FAILURE);
    }


    /* we need to exit the program before calling daemon() with
     * exit(EXIT_FAILURE) if we can not write to the pid file. So the init
     * script gets a failure return value and is able to show it to the user.
     *
     * But we need to write the pid value _after_ calling daemon(), so the pid
     * contains the real pid value.
     *
     * Combine both we need to open the pid file before calling daemon() and
     * write to the file after calling daemon().
     */

    {
	FILE *pidfile = NULL;
	const char *pidpath = config_get_pid_path();
	if (pidpath)
	{
	    // TODO try to create a path in basename(pidfile) with root permissions?
	    pidfile = fopen(pidpath, "w");
	    if (NULL == pidfile)
	    {
		logerror("ERROR: can not open pidfile '%s': ", pidpath);
		exit(EXIT_FAILURE);
	    }
	}

	if ( !no_daemon )
	{
	    retval = daemon(daemon_no_change_dir,daemon_no_close_streams);
	    if (retval)
	    {
		logerror("ERROR: can not become daemon");
		exit(EXIT_FAILURE);
	    }

	}

	if (pidpath)
	{
	    pid_t pid = getpid();
	    retval = fprintf(pidfile, "%d", pid);
	    if (0 > retval)
	    {
		logerror("ERROR: can not write to pidfile '%s': ", pidpath);
	    }
	    fclose(pidfile);
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
	    logerror("ERROR: can not install signal pipe");
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
	static const int stdactionflags = SA_SIGINFO|SA_NOCLDSTOP|SA_NOCLDWAIT;
	action.sa_flags = stdactionflags;
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGCHLD);
	sigaddset(&action.sa_mask, SIGHUP);
	sigaddset(&action.sa_mask, SIGUSR1);
	sigaddset(&action.sa_mask, SIGUSR2);
	sigaddset(&action.sa_mask, SIGTERM);
	sigaddset(&action.sa_mask, SIGINT);
	sigaddset(&action.sa_mask, SIGQUIT);
	retval = sigaction(SIGUSR1, &action, NULL);
	if (retval)
	{
	    logerror("ERROR: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	retval = sigaction(SIGUSR2, &action, NULL);
	if (retval)
	{
	    logerror("ERROR: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	retval = sigaction(SIGTERM, &action, NULL);
	if (retval)
	{
	    logerror("ERROR: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	retval = sigaction(SIGQUIT, &action, NULL);
	if (retval)
	{
	    logerror("ERROR: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	retval = sigaction(SIGHUP, &action, NULL);
	if (retval)
	{
	    logerror("ERROR: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	retval = sigaction(SIGINT, &action, NULL);
	if (retval)
	{
	    logerror("ERROR: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	retval = sigaction(SIGCHLD, &action, NULL);
	if (retval)
	{
	    logerror("ERROR: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
	action.sa_flags = stdactionflags;
	action.sa_flags |= SA_RESETHAND; // only one notification of sigsegv
	retval = sigaction(SIGSEGV, &action, NULL);
	if (retval)
	{
	    logerror("ERROR: can not install signal handler");
	    exit(EXIT_FAILURE);
	}
    }



    /* start the inotify watch module */
    qgis_inotify_init();

    /* start the process shutdown module */
    qgis_shutdown_init(signalpipe_wr);

    /* start the child processes */
//    project_manager_startup_projects();
    project_manager_manage_project_changes((const char **)sectionnew, (const char **)sectionchange, (const char **)sectiondelete);
    config_delete_section_change_list(sectionnew);
    config_delete_section_change_list(sectionchange);
    config_delete_section_change_list(sectiondelete);



    /* wait for signals of child processes exiting (SIGCHLD) or to terminate
     * this program (SIGTERM, SIGINT) or clients connecting via network to
     * this server.
     */

    enum {
	serverfd_slot = 0,
	pipefd_slot = 1,
	num_poll_slots
    };
    struct pollfd pfd[num_poll_slots];

    pfd[serverfd_slot].fd = serversocketfd;
    pfd[pipefd_slot].fd = signalpipe_rd;

    int has_finished = 0;
    int has_restored_signal = 0;
    int is_readable_serversocket = 0;
    int is_readable_signalpipe = 0;
    printlog("Initialization done. Waiting for network connection requests..");
    while ( !has_finished )
    {
	/* wait for connections or signals */
	if (!is_readable_serversocket)
	    pfd[serverfd_slot].events = POLLIN;
	else
	    pfd[serverfd_slot].events = 0;
	if (!is_readable_signalpipe)
	    pfd[pipefd_slot].events = POLLIN;
	else
	    pfd[pipefd_slot].events = 0;

	retval = poll(pfd, num_poll_slots, -1);
	if (-1 == retval)
	{
	    switch (errno)
	    {
	    case EINTR:
		/* We received an interrupt, possibly a termination signal.
		 * Let the main thread clean up: Wait for all child processes
		 * to end, close all remaining file descriptors and exit.
		 */
		debug(1, "received interrupt");
		break;

	    default:
		logerror("ERROR: %s() calling poll", __FUNCTION__);
		exit(EXIT_FAILURE);
		// no break needed
	    }
	}


	if (retval > 0)
	{
	    if (POLLIN & pfd[serverfd_slot].revents)
	    {
		is_readable_serversocket = 1;
		debug(1, "can read from network socket");
	    }
	    if(POLLIN & pfd[pipefd_slot].revents)
	    {
		is_readable_signalpipe = 1;
		debug(1, "can read from pipe");
	    }

	    if (is_readable_signalpipe)
	    {
		/* signal has been send to this thread */
		struct signal_data_s sigdata;
		retval = read(signalpipe_rd, &sigdata, sizeof(sigdata));
		if (-1 == retval)
		{
		    logerror("ERROR: reading signal data");
		    exit(EXIT_FAILURE);
		}
		else
		{
		    debug(1, "-- read %d bytes, got signal %d, child %d", retval, sigdata.signal, sigdata.pid);

		    /* react on signals */
		    switch (sigdata.signal)
		    {
		    case SIGCHLD:
		    {
			/* child process died, rearrange the project list */
			process_manager_process_died();
			break;
		    }
		    case SIGUSR1:
			statistic_printlog();
			break;

		    case SIGUSR2:
			db_dump();
			break;

		    case SIGTERM:	// fall through
		    case SIGINT:
		    case SIGQUIT:
			/* termination signal, kill all child processes */
			debug(1, "got termination signal, exit program");
			set_program_shutdown(1);

			/* shut down all projects */
			project_manager_shutdown();

			/* wait for the shutdown module so it has closed all its processes
			 * Then clean up the module */
			qgis_shutdown_wait_empty();

			/* close the inotify module, so no processes are recreated afterwards
			 * because of a change in the configuration.
			 */
			qgis_inotify_delete();

			// TODO restore default signal handler over here not below
			break;

		    case SIGHUP:
			/* hang up signal, reload configuration */
			printlog("received SIGHUP, reloading configuration");
			config_load(configuration_path, &sectionnew, &sectionchange, &sectiondelete);
			project_manager_manage_project_changes((const char **)sectionnew, (const char **)sectionchange, (const char **)sectiondelete);
			config_delete_section_change_list(sectionnew);
			config_delete_section_change_list(sectionchange);
			config_delete_section_change_list(sectiondelete);
			break;

		    case 0:
			if (sigdata.is_shutdown)
			{
			    debug(1, "got signal from shutdown module, exit");
			    has_finished = 1;
			}
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
			logerror("ERROR: calling accept");
			exit(EXIT_FAILURE);
		    }
		    else
		    {
			int networkfd = retval;
			connection_manager_handle_connection_request(networkfd, &addr, addrlen);
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
	if ( get_program_shutdown() && !has_restored_signal )
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
		logerror("ERROR: can not install signal handler");
		exit(EXIT_FAILURE);
	    }
	    retval = sigaction(SIGQUIT, &action, NULL);
	    if (retval)
	    {
		logerror("ERROR: can not install signal handler");
		exit(EXIT_FAILURE);
	    }
	    retval = sigaction(SIGINT, &action, NULL);
	    if (retval)
	    {
		logerror("ERROR: can not install signal handler");
		exit(EXIT_FAILURE);
	    }

	    has_restored_signal = 1;
	}

    }


    debug(1, "closing network socket");
    fflush(stderr);
    retval = close(serversocketfd);
    debug(1, "closed internet server socket fd %d, retval %d, errno %d", serversocketfd, retval, errno);

    /* wait for the shutdown module so it has closed all its processes
     * Then clean up the module */
    qgis_shutdown_delete();

    {
	const char *pidfile = config_get_pid_path();
	if (pidfile)
	{
	    remove_pid_file(pidfile);
	}
    }
    db_delete();
    config_shutdown();

    /* close the pipe */
    close(signalpipe_rd);
    close(signalpipe_wr);
    signalpipe_rd = signalpipe_wr = -1;
    /* delete remaining memory */
    free(configuration_path);

    printlog("shut down %s", basename(argv[0]));

    return exitvalue;
}
