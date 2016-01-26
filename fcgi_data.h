/*
 * fcgi_data.h
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


#ifndef FCGI_DATA_H_
#define FCGI_DATA_H_


struct fcgi_data_s;
struct fcgi_data_list_s;
struct fcgi_data_list_iterator_s;


struct fcgi_data_list_s *fcgi_data_list_new(void);
void fcgi_data_list_delete(struct fcgi_data_list_s *datalist);

void fcgi_data_add_data(struct fcgi_data_list_s *datalist, char *data, int len);
const char *fcgi_data_get_data(const struct fcgi_data_s *data);
int fcgi_data_get_datalen(const struct fcgi_data_s *data);

struct fcgi_data_list_iterator_s *fcgi_data_get_iterator(struct fcgi_data_list_s *list);
int fcgi_data_iterator_has_data(const struct fcgi_data_list_iterator_s *iterator);
const struct fcgi_data_s *fcgi_data_get_next_data(struct fcgi_data_list_iterator_s **iterator);




#endif /* FCGI_DATA_H_ */
