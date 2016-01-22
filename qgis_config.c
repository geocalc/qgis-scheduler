/*
 * qgis_config.c
 *
 *  Created on: 15.01.2016
 *      Author: jh
 */

/*
    Database for the scheduler configuration.

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



#include "qgis_config.h"

#include <stdint.h>
#include <assert.h>
#include <iniparser.h>


#define CONFIG_LISTEN_KEY		":listen"
#define DEFAULT_CONFIG_LISTEN_VALUE	"*"
#define CONFIG_PORT_KEY			":port"
#define DEFAULT_CONFIG_PORT_VALUE	"10177"
#define CONFIG_USER_KEY			":user"
#define DEFAULT_CONFIG_USER_VALUE	"root"
#define CONFIG_PROCESS_KEY		":process"
#define DEFAULT_CONFIG_PROCESS_VALUE	NULL
#define CONFIG_PROCESS_ARGS_KEY		":process_args"
#define DEFAULT_CONFIG_PROCESS_ARGS_VALUE	NULL
#define CONFIG_MIN_PROCESS		":min_proc"
#define DEFAULT_CONFIG_MIN_PROCESS	1
#define CONFIG_MAX_PROCESS		":max_proc"
#define DEFAULT_CONFIG_MAX_PROCESS	20
#define CONFIG_SCAN_PARAM		":scan_param"
#define DEFAULT_CONFIG_SCAN_PARAM	NULL
#define CONFIG_SCAN_REGEX		":scan_regex"
#define DEFAULT_CONFIG_SCAN_REGEX	NULL



#if __WORDSIZE == 64
# define INT_MIN	INT64_MIN
#elif __WORDSIZE == 32
# define INT_MIN	INT32_MIN
#else
# error unknown __WORDSIZE defined
#endif




static dictionary *config_opts = NULL;


/* Copy content of s1 and s2 into a new allocated string.
 * You have to free() the resulting string yourself.
 */
char *astrcat(const char *s1, const char *s2)
{
    assert(s1);
    assert(s2);
    int len1 = strlen(s1);
    int len2 = strlen(s2);

    char *astr = malloc(len1+len2+1);
    strcpy(astr, s1);
    strcat(astr, s2);

    return astr;
}


int config_load(const char *path)
{
    /* load the config file into the dictionary
     * return: 0 if all is well, -1 if not and errno is set
     */
    assert(path);
    config_opts = iniparser_load(path);
    if (!config_opts)
	return -1;

    return 0;
}


void config_shutdown(void)
{
    // no assert: it's ok to call this with config_opts==NULL
    iniparser_freedict(config_opts);
    config_opts = NULL;
}


int config_get_num_projects(void)
{
    assert(config_opts);
    return iniparser_getnsec(config_opts);
}


char *config_get_name_project(int num)
{
    assert(config_opts);
    assert(num >= 0);
    return iniparser_getsecname(config_opts, num);
}


const char *config_get_network_listen(void)
{
    assert(config_opts);
    return iniparser_getstring(config_opts, CONFIG_LISTEN_KEY, DEFAULT_CONFIG_LISTEN_VALUE);
}


const char *config_get_network_port(void)
{
    assert(config_opts);
    return iniparser_getstring(config_opts, CONFIG_PORT_KEY, DEFAULT_CONFIG_PORT_VALUE);
}


const char *config_get_user(void)
{
    assert(config_opts);
    return iniparser_getstring(config_opts, CONFIG_USER_KEY, DEFAULT_CONFIG_USER_VALUE);
}


const char *config_get_process(const char *project)
{
    /* if project != NULL we first test the project section, then the
     * global section.
     * if project == NULL we take the global section */
    const char *retval = DEFAULT_CONFIG_PROCESS_VALUE;

    assert(config_opts);
    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_PROCESS_KEY);
	retval = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_PROCESS_VALUE);
	free (key);
    }

    if (DEFAULT_CONFIG_PROCESS_VALUE == retval)
	retval = iniparser_getstring(config_opts, CONFIG_PROCESS_KEY, DEFAULT_CONFIG_PROCESS_VALUE);

    return retval;
}


const char *config_get_process_args(const char *project)
{
    /* if project != NULL we first test the project section, then the
     * global section.
     * if project == NULL we take the global section */
    const char *retval = DEFAULT_CONFIG_PROCESS_ARGS_VALUE;

    assert(config_opts);
    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_PROCESS_ARGS_KEY);
	retval = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_PROCESS_ARGS_VALUE);
	free (key);
    }

    if (DEFAULT_CONFIG_PROCESS_ARGS_VALUE == retval)
	retval = iniparser_getstring(config_opts, CONFIG_PROCESS_ARGS_KEY, DEFAULT_CONFIG_PROCESS_ARGS_VALUE);

    return retval;
}


int config_get_min_idle_processes(const char *project)
{
    /* if project != NULL we first test the project section, then the
     * global section.
     * if project == NULL we take the global section */
    int retval;

    assert(config_opts);
    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_MIN_PROCESS);
	retval = iniparser_getint(config_opts, key, INT32_MIN);
	free (key);
	if (INT32_MIN != retval)
	    return retval;
    }

    retval = iniparser_getint(config_opts, CONFIG_MIN_PROCESS, DEFAULT_CONFIG_MIN_PROCESS);

    return retval;
}


int config_get_max_idle_processes(const char *project)
{
    /* if project != NULL we first test the project section, then the
     * global section.
     * if project == NULL we take the global section */
    int retval;

    assert(config_opts);
    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_MAX_PROCESS);
	retval = iniparser_getint(config_opts, key, INT32_MIN);
	free (key);
	if (INT32_MIN != retval)
	    return retval;
    }

    retval = iniparser_getint(config_opts, CONFIG_MAX_PROCESS, DEFAULT_CONFIG_MAX_PROCESS);

    return retval;
}


const char *config_get_scan_parameter_key(const char *project)
{
    /* if project != NULL we first test the project section.
     * if project == NULL we return default */
    const char *retval = DEFAULT_CONFIG_SCAN_PARAM;

    assert(config_opts);
    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_SCAN_PARAM);
	retval = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_SCAN_PARAM);
	free (key);
    }

    return retval;
}


const char *config_get_scan_parameter_regex(const char *project)
{
    /* if project != NULL we first test the project section.
     * if project == NULL we return default */
    const char *retval = DEFAULT_CONFIG_SCAN_REGEX;

    assert(config_opts);
    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_SCAN_REGEX);
	retval = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_SCAN_REGEX);
	free (key);
    }

    return retval;
}






