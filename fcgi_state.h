/*
 * fcgi_state.h
 *
 *  Created on: 08.01.2016
 *      Author: jh
 */

/*
    Fast CGI state tracker.
    Parses the information flow during a fcgi session. Tracks the state.
    Gives information about the session state.

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


#ifndef FCGI_STATE_H_
#define FCGI_STATE_H_

#include <stdint.h>

/* error codes */
#define EWRONGSESSION	256

enum fcgi_session_state_e
{
    FCGI_SESSION_STATE_INIT = 0,
    FCGI_SESSION_STATE_RUNNING,
    FCGI_SESSION_STATE_PARAMS_DONE,
    FCGI_SESSION_STATE_END,
    FCGI_SESSION_STATE_ERROR,
};



struct fcgi_session_s;
struct fcgi_message_s;

struct fcgi_session_s *fcgi_session_new(int keep_messages);
void fcgi_session_delete(struct fcgi_session_s *session);

/* builds up a fcgi session state until all data of 'len' is consumed */
int fcgi_session_parse(struct fcgi_session_s *session, const char *data, int len);
/* return: true (!=0) if message needs data, false (==0) if not */
int fcgi_session_need_more_data(struct fcgi_session_s *session);
/* return: session id for current session (>0),
 * 	0 if currently no session (i.e. FCGI_STATE_INIT)
 * 	-1 on error
 */
int fcgi_session_get_requestid(const struct fcgi_session_s *session);
int fcgi_session_get_role(const struct fcgi_session_s *session);

int fcgi_session_print(const struct fcgi_session_s *session);

const char *fcgi_session_get_param(const struct fcgi_session_s *session, const char *name);

enum fcgi_session_state_e fcgi_session_get_state(const struct fcgi_session_s *session);




struct fcgi_message_s *fcgi_message_new(void);
void fcgi_message_delete(struct fcgi_message_s *message);

/* parse given content into a fcgi message structure.
 * The message structure has to be initialized by fcgi_state_new_message().
 * return: bytes read
 * 	or 0 if message is complete parsed
 * 	or -1 on error.
 */
int fcgi_message_parse(struct fcgi_message_s *message, const char *data, int len);
/* return: message parsing is done (!=0) or not (==0) */
int fcgi_message_get_parse_done(const struct fcgi_message_s *message);
/* return: message type or -1 in case of error */
int fcgi_message_get_requestid(const struct fcgi_message_s *message);
/* return: message type or -1 in case of error */
int fcgi_message_get_type(const struct fcgi_message_s *message);
/* return: message role or -1 in case of error */
int fcgi_message_get_role(const struct fcgi_message_s *message);
/* return: message flag or -1 in case of error */
int fcgi_message_get_flag(const struct fcgi_message_s *message);
int fcgi_message_set_flag(struct fcgi_message_s *message, unsigned char flags);
int fcgi_message_write(char *buffer, int len, const struct fcgi_message_s *message);

/* construct a message */
struct fcgi_message_s *fcgi_message_new_begin(uint16_t requestId, uint16_t role, unsigned char flags);
struct fcgi_message_s *fcgi_message_new_parameter(uint16_t requestId, const char *parameter, uint16_t len);
struct fcgi_message_s *fcgi_message_new_stdin(uint16_t requestId, const char *stdindata, uint16_t len);
struct fcgi_message_s *fcgi_message_new_data(uint16_t requestId, const char *data, uint16_t len);
struct fcgi_message_s *fcgi_message_new_endrequest(uint16_t requestId, uint32_t appStatus, unsigned char protocolStatus);


int fcgi_param_list_write(char *buffer, int len, const char *name, const char *value);


#endif /* FCGI_STATE_H_ */

