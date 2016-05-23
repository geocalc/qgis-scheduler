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


#include "config.h"

#include "qgis_config.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <assert.h>
#include <iniparser.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/queue.h>
#include <libgen.h>

#include "logger.h"


#define CONFIG_LISTEN_KEY		":listen"
#define DEFAULT_CONFIG_LISTEN_VALUE	"*"
#define CONFIG_PORT_KEY			":port"
#define DEFAULT_CONFIG_PORT_VALUE	"10177"
#define CONFIG_CHUSER_KEY		":chuser"
#define DEFAULT_CONFIG_CHUSER_VALUE	NULL
#define CONFIG_CHROOT_KEY		":chroot"
#define DEFAULT_CONFIG_CHROOT_VALUE	NULL
#define CONFIG_PID_KEY			":pidfile"
#define DEFAULT_CONFIG_PID_VALUE	NULL
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
#define CONFIG_CWD			":cwd"
#define DEFAULT_CONFIG_CWD		"/"
#define CONFIG_PROJ_CONFIG_PATH		":config_file"
#define DEFAULT_CONFIG_PROJ_CONFIG_PATH	NULL
#define CONFIG_PROJ_INITVAR		":initkey"
#define DEFAULT_CONFIG_PROJ_INITVAR	NULL
#define CONFIG_PROJ_INITDATA		":initvalue"
#define DEFAULT_CONFIG_PROJ_INITDATA	NULL
#define CONFIG_PROJ_ENVVAR		":envkey"
#define DEFAULT_CONFIG_PROJ_ENVVAR	NULL
#define CONFIG_PROJ_ENVDATA		":envvalue"
#define DEFAULT_CONFIG_PROJ_ENVDATA	NULL
#define CONFIG_LOGFILE			":logfile"
#define DEFAULT_CONFIG_LOGFILE		NULL
#define CONFIG_DEBUGLEVEL		":debuglevel"
#define DEFAULT_CONFIG_DEBUGLEVEL	0
#define CONFIG_INCLUDE			":include"
#define DEFAULT_CONFIG_INCLUDE		NULL


#if __WORDSIZE == 64
# define INT_MIN	INT64_MIN
#elif __WORDSIZE == 32
# define INT_MIN	INT32_MIN
#else
# error unknown __WORDSIZE defined
#endif

#define INVALID_STRING	((char *)-1)

/* Default time to wait between sending SIGTERM and SIGKILL */
#define DEFAULT_PROCESS_SIGNAL_TIMEOUT_SEC	10
#define DEFAULT_PROCESS_SIGNAL_TIMEOUT_NANOSEC	0


#define STAILQ_APPEND_LIST_ENTRY(listpointer, elm, field)	\
    do {	\
	if (STAILQ_EMPTY(&(listpointer)->head))	\
	    STAILQ_INSERT_HEAD(&(listpointer)->head, elm, field);	\
	else	\
	    STAILQ_INSERT_TAIL(&(listpointer)->head, elm, field);	\
    } while(0)


struct sectionlist_s
{
    STAILQ_HEAD(listhead, sectioniterator_s) head;	/* Linked list head */
};

struct sectioniterator_s
{
    STAILQ_ENTRY(sectioniterator_s) entries;          /* Linked list prev./next entry */
    char *section;
};



const struct timespec default_signal_timeout =
{
	tv_sec: DEFAULT_PROCESS_SIGNAL_TIMEOUT_SEC,
	tv_nsec: DEFAULT_PROCESS_SIGNAL_TIMEOUT_NANOSEC
};



static dictionary *config_opts = NULL;
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;
static int does_program_shutdown = 0;
static clockid_t system_clk_id = 0;
static int debuglevel = DEFAULT_CONFIG_DEBUGLEVEL; // cache debuglevel, so we dont need mutex to read the level value (used to debug this module itself)



/* Copy content of s1 and s2 into a new allocated string.
 * You have to free() the resulting string yourself.
 */
#define astrcat(s1,s2)	anstrcat(2,s1,s2)


/* Copy content of s1, s2, ... into a new allocated string.
 * You have to free() the resulting string yourself.
 * Argument "n" describes the number of strings.
 */
static char *anstrcat(int n, ...)
{
    assert(n >= 0);
    int len = 0;
    int i;

    if (0 >= n)
	return NULL;

    va_list args, carg;
    va_start(args, n);
    va_copy(carg, args);

    // evaluate string length
    for (i=0; i<n; i++)
    {
	char *s = va_arg(args, char *);
	len += strlen(s);
    }
    va_end(args);

    // allocate fitting memory
    char *astr = malloc(len+1);
    *astr = '\0';

    // copy strings to memory
    for (i=0; i<n; i++)
    {
	char *s = va_arg(carg, char *);
	strcat(astr, s);
    }
    va_end(carg);


    return astr;
}


/* load an include file and append its section data to the temporary file "f"
 *
 */
static int load_include_file(FILE *f, const char *configpath)
{
    /* if "configpath" points to a directory iniparser_load() nevertheless
     * returns a value. BUT the dictionary remains empty. So if "configpath"
     * refers to a directory the file f does not grow further.
     */
    struct stat statbuff;
    int retval = stat(configpath, &statbuff);
    if (-1 == retval)
    {
	logerror("ERROR: calling stat() on '%s'", configpath);
	exit(EXIT_FAILURE);
    }

    if ((statbuff.st_mode & S_IFMT) == S_IFREG) {
	/* Handle regular file */

	dictionary *myconfig = iniparser_load(configpath);
	if (myconfig)
	{
	    iniparser_dump_ini(myconfig, f);
	    iniparser_freedict(myconfig);
	}
	else
	{
	    return -1;
	}
    }
    else
    {
	printlog("WARNING: included path '%s' does not refer to a regular file", configpath);
	errno = EINVAL;
	return -1;
    }

    return 0;
}


static int copy_config_file(FILE *tmpf, const char *configpath, const char *tmpfilename)
{
    int retval;

    FILE *configfile = fopen(configpath, "r");
    if (NULL == configfile)
    {
	logerror("ERROR: can not open configuration file '%s'", configpath);
	exit(EXIT_FAILURE);
    }

    static const int bufferlen = 4096;
    unsigned char buffer[bufferlen];
    while (!feof(configfile))
    {
	size_t len = fread(buffer, 1, bufferlen, configfile);
	if (ferror(configfile))
	{
	    logerror("ERROR: reading from '%s'", configpath);
	    exit(EXIT_FAILURE);
	}

	fwrite(buffer, 1, len, tmpf);
	if (ferror(tmpf))
	{
	    logerror("ERROR: writing to temporary file '%s'", tmpfilename);
	    exit(EXIT_FAILURE);
	}
    }

    retval = fclose(configfile);
    if (EOF == retval)
    {
	logerror("ERROR: closing file '%s'", configpath);
	exit(EXIT_FAILURE);
    }

    return 0;
}


static int glob_find_err(const char *epath, int eerrno)
{
    errno = eerrno;
    logerror("ERROR: glob testing path '%s'", epath);

    return 0;	// continue finding in glob(): return 0
}


static int glob_find_file(FILE *tmpf, const char *includepattern)
{
    int retval;

    glob_t globvec;
    retval = glob(includepattern, GLOB_NOSORT|GLOB_BRACE|GLOB_TILDE, glob_find_err, &globvec);
    debug(1, "glob returned %d", retval);
    switch (retval)
    {
    case GLOB_ABORTED:
	printlog("WARNING: file globbing aborted for configuration include '%s'", includepattern);
	break;

    case GLOB_NOMATCH:
	printlog("WARNING: no file found for configuration include '%s'", includepattern);
	break;

    case GLOB_NOSPACE:
	printlog("ERROR: glob can not allocate memory");
	exit(EXIT_FAILURE);

    case 0:
    {
	/* no errors */
	debug(1, "glob no errors");

	size_t i;
	for (i=0; i<globvec.gl_pathc; i++)
	{
	    retval = load_include_file(tmpf, globvec.gl_pathv[i]);
	    if (-1 == retval)
	    {
		logerror("WARNING: can not load included configuration file '%s'", globvec.gl_pathv[i]);
	    }
	}
	break;
    }

    default:
	printlog("ERROR: glob returned unknown error code %d", retval);
	exit(EXIT_FAILURE);
    }

    globfree(&globvec);

    return 0;
}


/* load the configuration file specified with "configpath" and all config files
 * specified in the main config file with "include=" setting.
 * returnes the new configuration.
 */
static dictionary *iniparser_load_with_include(const char *configpath)
{
    int retval;
    dictionary *config;

    assert(configpath);
    /* read in config file */
    config = iniparser_load(configpath);
    if (config)
    {
	/* make a first attempt to get the configured debug level, second is in config_load() */
	debuglevel = iniparser_getint(config, CONFIG_DEBUGLEVEL, DEFAULT_CONFIG_DEBUGLEVEL);

	/* test configuration for include setting */
	const char *includepathpattern = iniparser_getstring(config, CONFIG_INCLUDE, DEFAULT_CONFIG_INCLUDE);
	if (DEFAULT_CONFIG_INCLUDE != includepathpattern)
	{
	    /* got include setting.
	     * copy all configurations into one temporary file
	     * and again read this into memory.
	     * check for multiple paths with globbing
	     */
	    char *tmpfilename = tempnam(NULL, "qgis-schedulerd");
	    if ( !tmpfilename )
	    {
		printlog("ERROR: tmpnam() returned no temporary file name");
		exit(EXIT_FAILURE);
	    }

	    int tmpfd = open(tmpfilename, O_CREAT|O_WRONLY|O_EXCL|O_CLOEXEC, S_IRUSR|S_IWUSR);
	    if (-1 == tmpfd)
	    {
		logerror("ERROR: can not open temporary file %s", tmpfilename);
		exit(EXIT_FAILURE);
	    }

	    /* note: fdopen() argument "mode" have to reflect the settings in open() */
	    FILE *tmpf = fdopen(tmpfd, "w");
	    if (NULL == tmpf)
	    {
		logerror("ERROR: can not open file descriptor on temporary file %s", tmpfilename);
		exit(EXIT_FAILURE);
	    }

	    /* copy first configuration file "as is" including global
	     * configuration section to the temporary file.
	     */
	    retval = copy_config_file(tmpf, configpath, tmpfilename);

	    /* copy included configuration files sections only to the temporary
	     * file.
	     */
	    if ('/' == *includepathpattern)
	    {
		// absolute path
		retval = glob_find_file(tmpf, includepathpattern);
	    }
	    else
	    {
		// relative path
		char *pathcopy = strdup(configpath);
		char *dir = dirname(pathcopy);
		char *abspathpattern = anstrcat(3, dir, "/", includepathpattern);
		retval = glob_find_file(tmpf, abspathpattern);
		free(abspathpattern);
		free(pathcopy);
	    }

	    retval = fclose(tmpf);
	    if (EOF == retval)
	    {
		logerror("ERROR: can not close temporary file %s", tmpfilename);
		exit(EXIT_FAILURE);
	    }

	    iniparser_freedict(config);
	    config = iniparser_load(tmpfilename);
	    if (config)
	    {
		retval = unlink(tmpfilename);
		if (-1 == retval)
		{
		    logerror("ERROR: can not remove temporary file %s", tmpfilename);
		    exit(EXIT_FAILURE);
		}
	    }

	    free(tmpfilename);
	}
    }


    if (!config)
    {
	logerror("ERROR: can not load configuration from '%s'", configpath);
    }

    return config;
}


/* tests for differences in "section" of both dictionaries
 * return 0 if equal, 1 otherwise
 */
static int config_test_for_section_change(dictionary *oldconfig, dictionary *newconfig, char *section)
{
    const int n = iniparser_getsecnkeys(oldconfig, section);
    int i = iniparser_getsecnkeys(newconfig, section);
    if (n != i)
	// different count of keys in configs
	return 1;

    /* section count equals in both dictionaries.
     * check for differences in key names or key values.
     */
    int k;
    char **oldkeys = iniparser_getseckeys(oldconfig, section);
    char **newkeys = iniparser_getseckeys(newconfig, section);
    for (i=0; i<n; i++)
    {
	const char *oldkey = oldkeys[i];
	/* try to find equal key in new keys */
	const char *newkey = NULL;
	for (k=0; k<n; k++)
	{
	    if (! strcmp(oldkey, newkeys[k]))
	    {
		// keys are equal
		newkey = newkeys[k];
		break;
	    }
	}

	if (newkey)
	{
	    /* keys are equal. values as well? */
	    const char *oldvalue = iniparser_getstring(oldconfig, oldkey, "");
	    const char *newvalue = iniparser_getstring(newconfig, newkey, "");
	    if (strcmp(oldvalue, newvalue))
		// values differ
		return 1;
	}
	else
	{
	    /* found no equal key in new config?
	     * configs differ, return 1
	     */
	    debug(1, "did not find key '%s' in new dictionary. dictionaries differ", oldkey);
	    return 1;
	}
    }
    free(oldkeys);
    free(newkeys);

    return 0;
}


/* Scans the configuration (global and projects) for differences to the
 * existing configuration.
 * If global variables differ, then all projects are reloaded.
 * If a project specific variable changed only that project is reloaded.
 *
 * return 0 if config does not differ, 1 otherwise
 */
static int config_has_changed(dictionary *oldconfig, dictionary *newconfig, struct sectionlist_s *sectionnew, struct sectionlist_s *sectionchanged, struct sectionlist_s *sectiondelete)
{
    int retval;
    /* check the globals */
    /* TODO: test known values for changes and react specific upon changes.
     *       If "min_proc" or "max_proc" change then start or shutdown
     *       individual processes.
     *       If "process" changed, then restart all processes. If "envkey"
     *       or "envvalue" changed as well.
     */

    /* check the projects */
    /* scan all old projects if the still exist in the new config.
     * if not then shutdown the old project.
     * if its configuration has changed reload the project.
     */
    int n = iniparser_getnsec(oldconfig);
    int i;
    for (i=0; i<n; i++)
    {
	char *secname = iniparser_getsecname(oldconfig, i);
	/* test if section exists in the new config.
	 * NOTE: iniparser_find_entry() expects the section name to contain the
	 *       name only (like "rwe") with no colon appended.
	 */
	retval = iniparser_find_entry(newconfig, secname);
	if (retval)
	{
	    /* section exist in old and new config.
	     * test for differences.
	     */
	    retval = config_test_for_section_change(oldconfig, newconfig, secname);
	    if (retval)
	    {
		/* section differs from old to new config.
		 */
		debug(1, "config differ section '%s' changed", secname);
		struct sectioniterator_s *element = malloc(sizeof(*element));
		assert(element);
		if ( !element )
		{
		    logerror("ERROR: could not allocate memory");
		    exit(EXIT_FAILURE);
		}
		element->section = strdup(secname);
		STAILQ_APPEND_LIST_ENTRY(sectionchanged, element, entries);
	    }
	}
	else
	{
	    /* section exist in old config only.
	     */
	    debug(1, "config differ section '%s' deleted", secname);
	    struct sectioniterator_s *element = malloc(sizeof(*element));
	    assert(element);
	    if ( !element )
	    {
		logerror("ERROR: could not allocate memory");
		exit(EXIT_FAILURE);
	    }
	    element->section = strdup(secname);
	    STAILQ_APPEND_LIST_ENTRY(sectiondelete, element, entries);
	}
    }

    /* scan the new config if it contains a new section.
     * if it does startup a new project
     */
    n = iniparser_getnsec(newconfig);
    for (i=0; i<n; i++)
    {
	const char *secname = iniparser_getsecname(newconfig, i);
	/* test if section exists in the old config.
	 * NOTE: iniparser_find_entry() expects the section name to contain the
	 *       name only (like "rwe") with no colon appended.
	 */
	retval = iniparser_find_entry(oldconfig, secname);
	if (retval)
	{
	    /* section exist in old and new config.
	     * has been handled before, no action needed.
	     */
	    ;
	}
	else
	{
	    /* section exist in new config only.
	     */
	    debug(1, "config differ section '%s' new", secname);
	    struct sectioniterator_s *element = malloc(sizeof(*element));
	    assert(element);
	    if ( !element )
	    {
		logerror("ERROR: could not allocate memory");
		exit(EXIT_FAILURE);
	    }
	    element->section = strdup(secname);
	    STAILQ_APPEND_LIST_ENTRY(sectionnew, element, entries);
	}
    }

    return 0;
}


/* take a single linked list and converts it to an array of string pointers.
 * the content of "list" is deleted during the call, the head of "list" is not.
 * returns the array in "*array", the array is NULL terminated.
 */
static void config_convert_list_to_array(char ***array, struct sectionlist_s *list)
{
    assert(array);
    assert(list);

    int n = 0;
    struct sectioniterator_s *it;
    STAILQ_FOREACH(it, &list->head, entries)
    {
	n++;
    }

    *array = calloc(n+1, sizeof(**array)); // last entry is NULL terminator
    int i = 0;
    while( !STAILQ_EMPTY(&list->head) )
    {
	assert(i < n);
	it = STAILQ_FIRST(&list->head);
	(*array)[i] = it->section;

	STAILQ_REMOVE_HEAD(&list->head, entries);
	free(it);
    }

}



/* =======================================================
 * public API
 */

/* Read the configuration files with one include value.
 * If the first configuration file has an entry "include" in the global section
 * (section without a name, i.e. key ":include") it reads the given
 * configuration file(s), too. */
// The include may contain wildcards (like "/etc/config/*.conf" or even
// "/etc/*qgis/*.conf".
/* The included file may contain a global section, which is being ignored. This
 * includes any further include values as well. So there can only one include
 * value be set.
 * The included configuration sections are written to a temporary file, which
 * in turn is read in a final pass from the "iniparser" library.
 *
 * Rereads a new config file with includes if being called a second time.
 * Scans the configuration (global and projects) for differences to the
 * existing configuration.
 * If global variables differ, then all projects are reloaded.
 * If a project specific variable changed only that project is reloaded.
 * The sections which the caller need to act upon are returned in "sectionnew",
 * "sectionchanged" and "sectiondelete". All arrays are NULL terminated.
 */
int config_load(const char *path, char ***sectionnew, char ***sectionchanged, char ***sectiondelete)
{
    /* load the config file into the dictionary
     * return: 0 if all is well, -1 if not and errno is set
     */
    assert(path);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    /* load the config file(s) */
    {
	dictionary *newconfig = iniparser_load_with_include(path);
	/* different handling if previously a configuration has been loaded:
	 * if no previous config exists and this load attempt did not succeed exit with error.
	 * if no previous config exists and this load has succeeded set the new config.
	 * if a previous config exists and this load attempt did not succeed print a warning and continue.
	 * if a previous config exists and this load has succeeded scan for differences and act upon.
	 */
	if (config_opts)
	{
	    if (NULL == newconfig)
	    {
		logerror("WARNING: could not load configuration file '%s'", path);
		*sectionnew = *sectionchanged = *sectiondelete = NULL;
	    }
	    else
	    {
		struct sectionlist_s listnew, listchanged, listdelete;
		STAILQ_INIT(&listnew.head);
		STAILQ_INIT(&listchanged.head);
		STAILQ_INIT(&listdelete.head);

		config_has_changed(config_opts, newconfig, &listnew, &listchanged, &listdelete);
		iniparser_freedict(config_opts);
		config_opts = newconfig;

		config_convert_list_to_array(sectionnew, &listnew);
		config_convert_list_to_array(sectionchanged, &listchanged);
		config_convert_list_to_array(sectiondelete, &listdelete);
	    }

	}
	else
	{
	    if (NULL == newconfig)
	    {
		logerror("ERROR: could not load configuration file '%s'", path);
		exit(EXIT_FAILURE);
	    }
	    else
	    {
		config_opts = newconfig;

		const int n = iniparser_getnsec(config_opts);

		*sectionnew = calloc(n+1, sizeof(**sectionnew));

		int i;
		for (i=0; i<n; i++)
		{
		    (*sectionnew)[i] = strdup(iniparser_getsecname(config_opts, i));
		}

		*sectionchanged = *sectiondelete = NULL;
	    }
	}
    }

    /* make a second attempt to get the configured debug level, first is in iniparser_load_with_include() */
    debuglevel = iniparser_getint(config_opts, CONFIG_DEBUGLEVEL, DEFAULT_CONFIG_DEBUGLEVEL);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }
    if (!config_opts)
	return -1;


    return 0;
}


void config_shutdown(void)
{
    // no assert: it's ok to call this with config_opts==NULL

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    iniparser_freedict(config_opts);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    config_opts = NULL;
}


int config_get_num_projects(void)
{
    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    int ret = iniparser_getnsec(config_opts);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_name_project(int num)
{
    assert(config_opts);
    assert(num >= 0);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    const char *ret = iniparser_getsecname(config_opts, num);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_network_listen(void)
{
    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    const char *ret = iniparser_getstring(config_opts, CONFIG_LISTEN_KEY, DEFAULT_CONFIG_LISTEN_VALUE);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_network_port(void)
{
    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    const char *ret = iniparser_getstring(config_opts, CONFIG_PORT_KEY, DEFAULT_CONFIG_PORT_VALUE);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_chuser(void)
{
    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    const char *ret = iniparser_getstring(config_opts, CONFIG_CHUSER_KEY, DEFAULT_CONFIG_CHUSER_VALUE);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_chroot(void)
{
    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    const char *ret = iniparser_getstring(config_opts, CONFIG_CHROOT_KEY, DEFAULT_CONFIG_CHROOT_VALUE);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_pid_path(void)
{
    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    const char *ret = iniparser_getstring(config_opts, CONFIG_PID_KEY, DEFAULT_CONFIG_PID_VALUE);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_logfile(void)
{
    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    const char *ret = iniparser_getstring(config_opts, CONFIG_LOGFILE, DEFAULT_CONFIG_LOGFILE);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


int config_get_debuglevel(void)
{
    // return cached entry read during config_load()
    return debuglevel;
}



const char *config_get_process(const char *project)
{
    /* if project != NULL we first test the project section, then the
     * global section.
     * if project == NULL we take the global section */
    const char *ret = DEFAULT_CONFIG_PROCESS_VALUE;

    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_PROCESS_KEY);
	ret = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_PROCESS_VALUE);
	free (key);
    }

    if (DEFAULT_CONFIG_PROCESS_VALUE == ret)
	ret = iniparser_getstring(config_opts, CONFIG_PROCESS_KEY, DEFAULT_CONFIG_PROCESS_VALUE);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_process_args(const char *project)
{
    /* if project != NULL we first test the project section, then the
     * global section.
     * if project == NULL we take the global section */
    const char *ret = DEFAULT_CONFIG_PROCESS_ARGS_VALUE;

    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_PROCESS_ARGS_KEY);
	ret = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_PROCESS_ARGS_VALUE);
	free (key);
    }

    if (DEFAULT_CONFIG_PROCESS_ARGS_VALUE == ret)
	ret = iniparser_getstring(config_opts, CONFIG_PROCESS_ARGS_KEY, DEFAULT_CONFIG_PROCESS_ARGS_VALUE);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


int config_get_min_idle_processes(const char *project)
{
    /* if project != NULL we first test the project section, then the
     * global section.
     * if project == NULL we take the global section */
    int ret = INT32_MIN;

    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_MIN_PROCESS);
	ret = iniparser_getint(config_opts, key, INT32_MIN);
	free (key);
    }

    if (INT32_MIN == ret)
	ret = iniparser_getint(config_opts, CONFIG_MIN_PROCESS, DEFAULT_CONFIG_MIN_PROCESS);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


int config_get_max_idle_processes(const char *project)
{
    /* if project != NULL we first test the project section, then the
     * global section.
     * if project == NULL we take the global section */
    int ret = INT32_MIN;

    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_MAX_PROCESS);
	ret = iniparser_getint(config_opts, key, INT32_MIN);
	free (key);
    }

    if (INT32_MIN == ret)
	ret = iniparser_getint(config_opts, CONFIG_MAX_PROCESS, DEFAULT_CONFIG_MAX_PROCESS);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_scan_parameter_key(const char *project)
{
    /* if project != NULL we first test the project section.
     * if project == NULL we return default */
    const char *ret = DEFAULT_CONFIG_SCAN_PARAM;

    assert(config_opts);

    if (project)
    {
	int retval = pthread_mutex_lock(&config_lock);
	if (retval)
	{
	    errno = retval;
	    logerror("ERROR: acquire mutex lock");
	    exit(EXIT_FAILURE);
	}

	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_SCAN_PARAM);
	ret = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_SCAN_PARAM);
	free (key);


	retval = pthread_mutex_unlock(&config_lock);
	if (retval)
	{
	    errno = retval;
	    logerror("ERROR: unlock mutex lock");
	    exit(EXIT_FAILURE);
	}
    }

    return ret;
}


const char *config_get_scan_parameter_regex(const char *project)
{
    /* if project != NULL we first test the project section.
     * if project == NULL we return default */
    const char *ret = DEFAULT_CONFIG_SCAN_REGEX;

    assert(config_opts);

    if (project)
    {
	int retval = pthread_mutex_lock(&config_lock);
	if (retval)
	{
	    errno = retval;
	    logerror("ERROR: acquire mutex lock");
	    exit(EXIT_FAILURE);
	}

	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_SCAN_REGEX);
	ret = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_SCAN_REGEX);
	free (key);


	retval = pthread_mutex_unlock(&config_lock);
	if (retval)
	{
	    errno = retval;
	    logerror("ERROR: unlock mutex lock");
	    exit(EXIT_FAILURE);
	}
    }

    return ret;
}


const char *config_get_working_directory(const char *project)
{
    /* if project != NULL we first test the project section, then the
     * global section.
     * if project == NULL we take the global section */
    const char *ret = DEFAULT_CONFIG_CWD;

    assert(config_opts);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_CWD);
	ret = iniparser_getstring(config_opts, key, INVALID_STRING);
	free (key);
    }

    if (INVALID_STRING == ret)
	ret = iniparser_getstring(config_opts, CONFIG_CWD, DEFAULT_CONFIG_CWD);

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;

}


const char *config_get_project_config_path(const char *project)
{
    /* if project != NULL we first test the project section.
     * if project == NULL we return default */
    const char *ret = DEFAULT_CONFIG_PROJ_CONFIG_PATH;

    assert(config_opts);

    if (project)
    {
	int retval = pthread_mutex_lock(&config_lock);
	if (retval)
	{
	    errno = retval;
	    logerror("ERROR: acquire mutex lock");
	    exit(EXIT_FAILURE);
	}

	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key = astrcat(project, CONFIG_PROJ_CONFIG_PATH);
	ret = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_PROJ_CONFIG_PATH);
	free (key);

	retval = pthread_mutex_unlock(&config_lock);
	if (retval)
	{
	    errno = retval;
	    logerror("ERROR: unlock mutex lock");
	    exit(EXIT_FAILURE);
	}
    }

    return ret;
}


const char *config_get_init_key(const char *project, int num)
{
    const char *ret = DEFAULT_CONFIG_PROJ_INITVAR;

    assert(config_opts);
    assert(project);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key;
	retval = asprintf(&key, "%s%s%d", project, CONFIG_PROJ_INITVAR, num);
	if (-1 == retval)
	{
	    logerror("ERROR: asprintf");
	    exit(EXIT_FAILURE);
	}
	ret = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_PROJ_INITVAR);
	free (key);
    }

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_init_value(const char *project, int num)
{
    const char *ret = DEFAULT_CONFIG_PROJ_INITDATA;

    assert(config_opts);
    assert(project);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key;
	retval = asprintf(&key, "%s%s%d", project, CONFIG_PROJ_INITDATA, num);
	if (-1 == retval)
	{
	    logerror("ERROR: asprintf");
	    exit(EXIT_FAILURE);
	}
	ret = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_PROJ_INITDATA);
	free (key);
    }

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_env_key(const char *project, int num)
{
    const char *ret = INVALID_STRING;

    assert(config_opts);
    assert(project);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key;
	retval = asprintf(&key, "%s%s%d", project, CONFIG_PROJ_ENVVAR, num);
	if (-1 == retval)
	{
	    logerror("ERROR: asprintf");
	    exit(EXIT_FAILURE);
	}
	ret = iniparser_getstring(config_opts, key, INVALID_STRING);
	free (key);
    }

    if (INVALID_STRING == ret)
    {
	char *key;
	retval = asprintf(&key, "%s%d", CONFIG_PROJ_ENVVAR, num);
	if (-1 == retval)
	{
	    logerror("ERROR: asprintf");
	    exit(EXIT_FAILURE);
	}
	ret = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_PROJ_ENVVAR);
	free (key);
    }

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


const char *config_get_env_value(const char *project, int num)
{
    const char *ret = INVALID_STRING;

    assert(config_opts);
    assert(project);

    int retval = pthread_mutex_lock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: acquire mutex lock");
	exit(EXIT_FAILURE);
    }

    if (project)
    {
	/* NOTE: this could be faster if we use a local char array instead of
	 * dynamic memory. However this buffer maybe too small, to circumvent
	 * this we need to know the string sizes before. I think it is too much
	 * effort. Just use dynamic memory.
	 */
	char *key;
	retval = asprintf(&key, "%s%s%d", project, CONFIG_PROJ_ENVDATA, num);
	if (-1 == retval)
	{
	    logerror("ERROR: asprintf");
	    exit(EXIT_FAILURE);
	}
	ret = iniparser_getstring(config_opts, key, INVALID_STRING);
	free (key);
    }

    if (INVALID_STRING == ret)
    {
	char *key;
	retval = asprintf(&key, "%s%d", CONFIG_PROJ_ENVDATA, num);
	if (-1 == retval)
	{
	    logerror("ERROR: asprintf");
	    exit(EXIT_FAILURE);
	}
	ret = iniparser_getstring(config_opts, key, DEFAULT_CONFIG_PROJ_ENVDATA);
	free (key);
    }

    retval = pthread_mutex_unlock(&config_lock);
    if (retval)
    {
	errno = retval;
	logerror("ERROR: unlock mutex lock");
	exit(EXIT_FAILURE);
    }

    return ret;
}


void set_program_shutdown(int does_shutdown)
{
    does_program_shutdown = does_shutdown;
}


int get_program_shutdown(void)
{
    return does_program_shutdown;
}


/* checks which clock id can be used for a call to clock_gettime()
 * Do this in order of CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC_COARSE,
 * CLOCK_MONOTONIC, CLOCK_REALTIME_COARSE, CLOCK_REALTIME.
 */
void test_set_valid_clock_id(void)
{
    static const clockid_t clockidarr[] = {
	    /* Note: we use the same clock id in pthread_condattr_setclock()
	     * which seems to accept only CLOCK_MONOTONIC and CLOCK_REALTIME
	     */
	    /*CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC_COARSE,*/ CLOCK_MONOTONIC,
	    /*CLOCK_REALTIME_COARSE,*/ CLOCK_REALTIME};
    static const int numarr = sizeof(clockidarr)/sizeof(*clockidarr);
    int i;

    for (i=0; i<numarr; i++)
    {
	int retval = clock_getres(clockidarr[i], NULL);
	if (-1 == retval)
	{
	    if (EINVAL != errno)
	    {
		logerror("ERROR: clock_getres(%d, NULL)", clockidarr[i]);
	    }
	}
	else
	{
	    // success
	    system_clk_id = clockidarr[i];
	    return;
	}
    }

    if (errno)
	logerror("ERROR: can not get valid clockid");

}


void set_valid_clock_id(clockid_t clk_id)
{
    system_clk_id = clk_id;
}


clockid_t get_valid_clock_id(void)
{
    return system_clk_id;
}


/* delete the array of string pointers returned by config_load()
 */
void config_delete_section_change_list(char **array)
{
    if (array)
    {
	char **aptr = array;
	char *chrptr;
	while ((chrptr = *aptr++))
	{
	    free(chrptr);
	}
	free(array);
    }
}



