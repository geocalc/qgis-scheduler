/*
 * connection_manager.h
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


#ifndef CONNECTION_MANAGER_H_
#define CONNECTION_MANAGER_H_

struct sockaddr;
void connection_manager_handle_connection_request(int netfd, const struct sockaddr *addr, unsigned int length);


#endif /* CONNECTION_MANAGER_H_ */
