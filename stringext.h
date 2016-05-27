/*
 * stringext.h
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

#ifndef STRINGEXT_H_
#define STRINGEXT_H_


/* Copy content of s1 and s2 into a new allocated string.
 * You have to free() the resulting string yourself.
 */
#define astrcat(s1,s2)	anstrcat(2,s1,s2)

/* Copy content of s1, s2, ... into a new allocated string.
 * You have to free() the resulting string yourself.
 * Argument "n" describes the number of strings.
 */
char *anstrcat(int n, ...);


#endif /* STRINGEXT_H_ */
