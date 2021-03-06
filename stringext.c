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

#include "logger.h"
#include "qgis_shutdown_queue.h"


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
    if ( !astr )
    {
	/* malloc failed. exit */
	logerror("ERROR: malloc failed");
	qexit(EXIT_FAILURE);
    }
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


/* Like strncat() appends a string to an exiting string buffer. If the string
 * does not fit into the buffer it is resized to a new size. The new buffer is
 * stored into "buffer" and "len" is updated.
 */
void strnbcat(char **buffer, int *len, const char *str)
{
    assert(buffer);
    assert(len);
    assert(str);

    int appendlen = strlen(str);
    int bufferlen = strlen(*buffer);
    int newlen = *len;

    static const int max_resize_iteration = 10;
    int i;
    for (i=0; i<max_resize_iteration; i++)
    {
	// need newlen >=  (appendlen + bufferlen + 1) to fit into buffer
	if ((appendlen + bufferlen) >= newlen)
	{
	    /* "str" doesn't fit into "buffer". need to resize it
	     * Just double the size of the buffer */
	    newlen += newlen;
	}
	else
	{
	    break;
	}
    }
    if (i >= max_resize_iteration)
    {
	/* possible endless loop. exit with error */
	printlog("ERROR: possible endless loop in resize() algorithm. exit");
	qexit(EXIT_FAILURE);
    }
    if ( newlen != *len )
    {
	*len = newlen;
	*buffer = realloc(*buffer, newlen);
	if ( !*buffer )
	{
	    /* realloc failed. exit */
	    logerror("ERROR: realloc failed");
	    qexit(EXIT_FAILURE);
	}
    }
    strcat(*buffer, str);
}


/* Add a value to an array. Resize the array if all elements are filled.
 * 'dataarray' is a pointer to an array which is resized on demand.
 * 'nelem' holds the length of the array (measured in elements not bytes).
 * 'nlen' holds the used array elements.
 * 'data' is a pointer to the value of size 'sizeofdata'.
 */
void arraycat(void *dataarray, int *nelem, int *nlen, const void *data, int sizeofdata)
{
    assert(dataarray);
    assert(nelem);
    assert(nlen);
    assert(data);
    assert(sizeofdata > 0);

    /* construct a datatype with sizeof(struct datatype_s) == sizeofdata */
    typedef struct datatype_s
    {
	unsigned char dat[sizeofdata];
    } datatype_t;

    int myelem = *nelem;
    int mylen = *nlen;
    datatype_t *myarray = *((datatype_t **)dataarray);

    if (!myarray)	// reset all values if array is NULL
    {
	mylen = myelem = 0;
    }

    /* resize if the array is full */
    if (mylen >= myelem)
    {
	if (0 >= myelem)
	{
	    myelem = 16;	// set an initial array size of 16 elements
	    mylen = 0;
	}
	else
	    myelem *= 2;

	myarray = realloc(myarray, myelem*sizeof(*myarray));
	if ( !myarray )
	{
	    /* realloc failed. exit */
	    logerror("ERROR: realloc failed");
	    qexit(EXIT_FAILURE);
	}

	*((datatype_t **)dataarray) = myarray;
	*nelem = myelem;
    }

    myarray[mylen++] = *((datatype_t *)data);
    *nlen = mylen;
}


/* Like memcpy() appends data to an exiting buffer. If the data does not fit
 * into the buffer that buffer is resized to a new size. The new buffer is
 * stored into "buffer" and "buffersize" is updated.
 * Then the data is appended to the old buffer and "bufferlen" is updated.
 *
 * return: 0 on success, -1 otherwise
 */
int membcat(void **buffer, int *buffersize, int *bufferlen, const void *src, int srclen)
{
    assert(buffer);
    assert(buffersize);
    assert(*buffersize >= 0);
    assert(bufferlen);
    assert(*bufferlen >= 0);
    assert(src);
    assert(srclen >= 0);

    void *mybuffer = *buffer;
    unsigned int mybuffersize = *buffersize;
    unsigned int mybufferlen = *bufferlen;

    assert(mybuffersize >= mybufferlen);
    if ( !mybuffer )
	assert( !mybuffersize );


    int retval = 0;
    /* check the size needed to copy all data */
    unsigned int newbufferlen = mybufferlen + srclen;
    if (mybuffersize < newbufferlen)
    {
	/* bigger size needed.
	 * make new size 2^n with 2^n > newlen
	 */
	if (newbufferlen >= ((unsigned int)1)<<31) /* test for value > 2^30 */
	{
	    /* we can not use a bigger value because we can not store
	     * the value in a signed integer value (-2^31..+(2^31-1))
	     */
	    retval = -1; // unusable result, can not store in signed int type
	}
	else
	{
	    /* this is from https://graphics.stanford.edu/~seander/bithacks.html */

	    /* this code works with values below 2^31 */
	    unsigned int v = newbufferlen; // compute the next highest power of 2 of 32-bit v

	    v--;
	    v |= v >> 1;
	    v |= v >> 2;
	    v |= v >> 4;
	    v |= v >> 8;
	    v |= v >> 16;
	    v++;

	    if (v >= ((unsigned int)1)<<31) /* test for value > 2^31-1 */
		retval = -1; // unusable result, can not store in signed int type
	    else
		mybuffersize = v;
	}
    }

    if ( !retval )
    {
	if ( mybuffersize != *buffersize )
	{
	    mybuffer = realloc(mybuffer, mybuffersize);
	    if ( !mybuffer )
	    {
		/* realloc failed. exit */
		logerror("ERROR: realloc failed");
		qexit(EXIT_FAILURE);
	    }
	    *buffer = mybuffer;
	    *buffersize = mybuffersize;
	}

	memcpy(mybuffer + mybufferlen, src, srclen);
	*bufferlen = newbufferlen;
    }

    return retval;
}


