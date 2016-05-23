/*
 * fcgi_data.c
 *
 *  Created on: 26.01.2016
 *      Author: jh
 */

/*
    Simple list to store fcgi (or other) data.
    The data storage is handled in a queue. With the iterator we get the data
    in order.
    The data access is NOT thread safe.

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



#include "fcgi_data.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include <assert.h>

#include "logger.h"


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




struct fcgi_data_list_s *fcgi_data_list_new(void)
{
    struct fcgi_data_list_s *list = calloc(1, sizeof(*list));
    assert(list);
    if ( !list )
    {
	logerror("ERROR: could not allocate memory");
	exit(EXIT_FAILURE);
    }
    TAILQ_INIT(&list->head);

    return list;
}


void fcgi_data_list_delete(struct fcgi_data_list_s *datalist)
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


void fcgi_data_add_data(struct fcgi_data_list_s *datalist, const char *data, int len)
{
    assert(datalist);
    if(datalist)
    {
	struct fcgi_data_list_iterator_s *entry = malloc(sizeof(*entry));
	assert(entry);
	if ( !entry )
	{
	    logerror("ERROR: could not allocate memory");
	    exit(EXIT_FAILURE);
	}

	entry->data.data = malloc(len);
	if ( !entry->data.data )
	{
	    logerror("ERROR: could not allocate memory");
	    exit(EXIT_FAILURE);
	}
	memcpy(entry->data.data, data, len);
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


const char *fcgi_data_get_data(const struct fcgi_data_s *data)
{
    const char *retval = NULL;

    assert(data);
    if (data)
    {
	retval = data->data;
    }

    return retval;
}


int fcgi_data_get_datalen(const struct fcgi_data_s *data)
{
    int retval = 0;

    assert(data);
    if (data)
    {
	retval = data->len;
    }

    return retval;
}


struct fcgi_data_list_iterator_s *fcgi_data_get_iterator(struct fcgi_data_list_s *list)
{
    assert(list);
    if (list)
    {
	return list->head.tqh_first;
    }

    return NULL;
}


const struct fcgi_data_s *fcgi_data_get_next_data(struct fcgi_data_list_iterator_s **iterator)
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


int fcgi_data_iterator_has_data(const struct fcgi_data_list_iterator_s *iterator)
{
    int retval = 0;
    if (iterator)
    {
	retval = 1;
    }

    return retval;
}

