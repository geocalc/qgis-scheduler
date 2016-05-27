/*
 * stringext.c
 *
 *  Created on: 27.05.2016
 *      Author: jh
 */

/*
    Extended string handling.
    Provides extended functionality in string functions.

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

#include "stringext.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>


/* Copy content of s1, s2, ... into a new allocated string.
 * You have to free() the resulting string yourself.
 * Argument "n" describes the number of strings.
 */
char *anstrcat(int n, ...)
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


