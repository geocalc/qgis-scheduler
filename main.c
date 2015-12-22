/*
    Scheduler program for QGis server program. Talks FCGI with web server
    and FCGI with QGis server. Selects the correct server program based on
    the URL given from web-gis client program.

    Copyright (C) 2015  Jörg Habenicht (jh@mwerk.net)

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




#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <unistd.h>


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
    //fprintf(stdout, "\t-c: use CONFIGFILE (default '%s')\n", DEFAULT_CONFIG_PATH);
}


int main(int argc, char **argv)
{
    const int port = 10177;
    const int nr_childs = 10;
    const char command[] = "/usr/bin/qgis_mapserv.fcgi";
    int no_daemon = 0;

    int opt;

    while ((opt = getopt(argc, argv, "hd")) != -1)
    {
	switch (opt)
	{
	case 'h':
	    usage(argv[0]);
	    break;
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

    /* prepare inet socket connection for application server process (this)
     */
    int socketfd;

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
	    socketfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
	    if (socketfd == -1)
	    {
		//printf(" could not create socket\n");
		perror(" could not create socket");
		continue;
	    }

	    if (bind(socketfd, rp->ai_addr, rp->ai_addrlen) == 0)
		break; /* Success */

	    //printf(" could not bind to socket\n");
	    perror(" could not bind to socket");
	    close(socketfd);
	}

	if (rp == NULL)
	{ /* No address succeeded */
	    //fprintf(stderr, "Could not bind\n"); // TODO better message
	    perror("could not create socket");
	    exit(EXIT_FAILURE);
	}

	freeaddrinfo(result); /* No longer needed */
    }


    /* we are server. listen to incoming connections */
    int retval = listen(socketfd, SOMAXCONN);
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

    int i;
    for (i=0; i<nr_childs; i++)
    {
	int pid = fork();
	if (0 == pid)
	{
	    /* child */

	    /* close file descriptor stdin = 0
	     * assign socket file descriptor to fd 0
	     * fork
	     * exec
	     */
	    int ret = dup2(socketfd, 0);
	    if (-1 == ret)
	    {
		perror("error calling dup2");
		exit(EXIT_FAILURE);
	    }
//	    const char *command = iniparser_getstring(ini, CGI_PATH_KEY,
//		    CGI_PATH_KEY_DEFAULT);

	    execl(command, command, NULL);
	    fprintf(stderr, "could not execute '%s': ", command);
	    perror(NULL);
	    exit(EXIT_FAILURE);
	}
	else if (0 < pid)
	{
	    /* parent */
	}
	else
	{
	    /* error */
	    perror("can not fork");
	    exit(EXIT_FAILURE);
	}
    }


    return EXIT_SUCCESS;
}
