/*
 * common.h
 *
 *  Created on: 02.04.2016
 *      Author: jh
 */

/*
    Header for common and project wide definitions.

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


#ifndef COMMON_H_
#define COMMON_H_


/* return minimal value of a,b.
 * evaluate a and b only once.
 */
#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

/* return maximal value of a,b.
 * evaluate a and b only once.
 */
#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

/* stringify x
 * x may be a definition or a code line, too
 * #define MYDEF 0.1.abc
 * i.e. STR(MYDEF) => "0.1.abc", STR(a+b) => "a+b"
 */
#define _STR(x)	# x
#define STR(x)	_STR(x)

/* avoid compiler warning about unused parameter */
#define UNUSED_PARAMETER(x)	((void)(x))


/* do not use attribute setting outside the gcc world */
#ifndef __GNUC__
# define __attribute__(a)
#endif


#endif /* COMMON_H_ */
